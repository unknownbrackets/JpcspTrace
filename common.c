/*
This file is part of jpcsp.

Jpcsp is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Jpcsp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Jpcsp.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspinit.h>
#include <psploadcore.h>
#include <psputilsforkernel.h>
#include <pspsysmem_kernel.h>
#include <psprtc.h>
#include <string.h>
#include "common.h"

static const char *const hexDigits = "0123456789ABCDEF";
static const int logTimestamp = 1;
static const int logThreadName = 1;
static const int logRa = 0;
CommonInfo *commonInfo;
static void *freeAddr = NULL;
static int freeSize = 0;

#if DEBUG_MUTEX
typedef struct {
	SceSize size;
	char name[32];
	u32 attr;
	u32 initCount;
	u32 lockedCount;
	u32 threadid;
	u32 numWaitThreads;
} SceKernelMutexInfo;

int (* referMutex)(SceUID, SceKernelMutexInfo *) = NULL;
SceKernelMutexInfo mutexInfo;
#endif

void *alloc(int size) {
	void *allocAddr;

	size = ALIGN_UP(size, 4);

	if (commonInfo != NULL && commonInfo->freeAddr != NULL) {
		freeAddr = commonInfo->freeAddr;
		freeSize = commonInfo->freeSize;
	}

	if (freeSize >= size) {
		freeSize -= size;
		allocAddr = freeAddr + freeSize;
	} else {
		int allocSize = ALIGN_UP(size, 256);
		int result = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "JpcspTrace", PSP_SMEM_High, allocSize, 0);
		if (result >= 0) {
			void *newFreeAddr = sceKernelGetBlockHeadAddr(result);
			if (newFreeAddr + allocSize != freeAddr) {
				// Can't merge new allocated memory to previously allocated memory
				freeSize = 0;
			}
			freeAddr = newFreeAddr;
			freeSize += allocSize - size;
			allocAddr = freeAddr + freeSize;
		} else {
			allocAddr = NULL;
		}
	}

	if (commonInfo != NULL) {
		commonInfo->freeAddr = freeAddr;
		commonInfo->freeSize = freeSize;
	}

	return allocAddr;
}

char *append(char *dst, const char *src) {
	while ((*dst = *src) != '\0') {
		src++;
		dst++;
	}

	return dst;
}

char *appendHex(char *dst, u32 hex, int numDigits) {
	int i;
	*dst++ = '0';
	*dst++ = 'x';
	int leadingZero = 1;
	numDigits <<= 2;
	for (i = 28; i >= 0; i -= 4) {
		int digit = (hex >> i) & 0xF;
		if (digit > 0 || !leadingZero || i <= numDigits) {
			*dst++ = hexDigits[digit];
			leadingZero = 0;
		}
	}
	*dst = '\0';

	return dst;
}

char *appendInt(char *dst, s32 n, int numDigits) {
	if (n == 0) {
		*dst++ = '0';
		for (numDigits--; numDigits > 0; numDigits--) {
			*dst++ = '0';
		}
	} else {
		if (n < 0) {
			*dst++ = '-';
			n = -n;
		}

		int factor = 1000000000;
		int leadingZero = 1;
		int factorDigits = 10;
		while (factor > 0) {
			int digit = n / factor;
			if (digit > 0 || !leadingZero || factorDigits <= numDigits) {
				*dst++ = '0' + digit;
				n -= digit * factor;
				leadingZero = 0;
			}
			factor /= 10;
			factorDigits--;
		}
	}
	*dst = '\0';

	return dst;
}

void openLogFile() {
	if (commonInfo->logFd < 0) {
		commonInfo->logFd = sceIoOpen("ms0:/log.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	}
}

void closeLogFile() {
	sceIoClose(commonInfo->logFd);
	commonInfo->logFd = -1;
}

void appendToLogBuffer(const char *s, int length) {
	// Allocate a buffer if not yet allocated
	if (commonInfo->logBuffer == NULL) {
		commonInfo->logBuffer = alloc(commonInfo->maxLogBufferLength);
	}

	if (commonInfo->logBuffer != NULL) {
		int restLength = commonInfo->maxLogBufferLength - commonInfo->logBufferLength;
		int truncated = 0;
		if (length > restLength) {
			length = restLength;
			truncated = 1;
		}

		if (length > 0) {
			memcpy(commonInfo->logBuffer + commonInfo->logBufferLength, s, length);
			commonInfo->logBufferLength += length;

			// If we have truncated the string to be logged,
			// set "...\n" at the end of the log buffer.
			if (truncated) {
				char *addr = commonInfo->logBuffer + commonInfo->logBufferLength - 4;
				*addr++ = '.';
				*addr++ = '.';
				*addr++ = '.';
				*addr++ = '\n';
			}
		}
	}
}

void flushLogBuffer() {
	while (commonInfo->logBufferLength > 0) {
		// Try to write pending output.
		// This will succeed as soon as the interrupts are enabled again.
		int length = sceIoWrite(commonInfo->logFd, commonInfo->logBuffer, commonInfo->logBufferLength);
		if (length <= 0) {
			break;
		}

		commonInfo->logBufferLength -= length;
		if (commonInfo->logBufferLength > 0) {
			memcpy(commonInfo->logBuffer, commonInfo->logBuffer + length, commonInfo->logBufferLength);
		}
	}
}

static int dobuffering = 0;
void writeLog(const char *s, int length) {
	int forceBuffering = (dobuffering++ % 60) != 0;
	int restLength = commonInfo->maxLogBufferLength - commonInfo->logBufferLength;
	if (restLength < 512)
		forceBuffering = 0;
	if (commonInfo->inWriteLog || forceBuffering) {
		appendToLogBuffer(s, length);
		return;
	}

	commonInfo->inWriteLog++;

	if (!commonInfo->logKeepOpen) {
		openLogFile();
	}

	flushLogBuffer();

	if (sceIoWrite(commonInfo->logFd, s, length) < 0) {
		// Can't write to the log file right now, probably because the interrupts are disabled.
		// Save the log string for later output.
		appendToLogBuffer(s, length);
	} else {
		flushLogBuffer();
	}

	if (!commonInfo->logKeepOpen) {
		closeLogFile();
	}

	commonInfo->inWriteLog--;
}

void printLog(const char *s) {
	writeLog(s, strlen(s));
}

void printLogH(const char *s1, int hex, const char *s2) {
	char buffer[200];
	char *s = buffer;

	s = append(s, s1);
	s = appendHex(s, hex, 0);
	s = append(s, s2);
	writeLog(buffer, s - buffer);
}

void printLogS(const char *s1, const char *s2, const char *s3) {
	char buffer[200];
	char *s = buffer;

	s = append(s, s1);
	s = append(s, s2);
	s = append(s, s3);
	writeLog(buffer, s - buffer);
}

void printLogHH(const char *s1, int hex1, const char *s2, int hex2, const char *s3) {
	char buffer[200];
	char *s = buffer;

	s = append(s, s1);
	s = appendHex(s, hex1, 0);
	s = append(s, s2);
	s = appendHex(s, hex2, 0);
	s = append(s, s3);
	writeLog(buffer, s - buffer);
}

void printLogSH(const char *s1, const char *s2, const char *s3, int hex, const char *s4) {
	char buffer[200];
	char *s = buffer;

	s = append(s, s1);
	s = append(s, s2);
	s = append(s, s3);
	s = appendHex(s, hex, 0);
	s = append(s, s4);
	writeLog(buffer, s - buffer);
}

void printLogHS(const char *s1, int hex, const char *s2, const char *s3, const char *s4) {
	char buffer[200];
	char *s = buffer;

	s = append(s, s1);
	s = appendHex(s, hex, 0);
	s = append(s, s2);
	s = append(s, s3);
	s = append(s, s4);
	writeLog(buffer, s - buffer);
}

void printLogSS(const char *s1, const char *s2, const char *s3, const char *s4, const char *s5) {
	char buffer[200];
	char *s = buffer;

	s = append(s, s1);
	s = append(s, s2);
	s = append(s, s3);
	s = append(s, s4);
	s = append(s, s5);
	writeLog(buffer, s - buffer);
}

void printLogMem(const char *s1, int addr, int length) {
	int i, j;
	int lineStart;
	char buffer[100];
	char *s = buffer;

	s = append(s, s1);
	s = appendHex(s, addr, 8);
	s = append(s, ":\n");
	if (addr != 0) {
		lineStart = 0;
		for (i = 0; i < length; i += 4) {
			if (i > 0) {
				if ((i % 16) == 0) {
					s = append(s, "  >");
					for (j = lineStart; j < i; j++) {
						char c = _lb(addr + j);
						if (c < ' ' || c > '~') {
							c = '.';
						}
						*s++ = c;
					}
					s = append(s, "<\n");
					writeLog(buffer, s - buffer);
					s = buffer;
					lineStart = i;
				} else {
					s = append(s, ", ");
				}
			}
			s = appendHex(s, _lw(addr + i), 8);
		}
	}
	s = append(s, "\n");
	writeLog(buffer, s - buffer);
}

#ifdef DEBUG_UTILITY_SAVEDATA
void *utilitySavedataParams = NULL;

void utilitySavedataLog(char *buffer, const SyscallInfo *syscallInfo, u32 param) {
	char *s = buffer;

	if (syscallInfo->nid == 0x50C4CD57) {
		utilitySavedataParams = (void *) param;
	}

	int mode = _lw((int) utilitySavedataParams + 48);
	s = append(s, "mode=");
	s = appendInt(s, mode, 0);
	s = append(s, ", gameName=");
	s = append(s, utilitySavedataParams + 60);
	s = append(s, ", saveName=");
	s = append(s, utilitySavedataParams + 76);
	s = append(s, ", fileName=");
	s = append(s, utilitySavedataParams + 100);
	if (syscallInfo->nid == 0x9790B33C) {
		s = append(s, ", result=");
		s = appendHex(s, _lw((int) utilitySavedataParams + 28), 8);
	}
	s = append(s, "\n");
	writeLog(buffer, s - buffer);
	s = buffer;

	printLogMem("Data ", _lw((int) utilitySavedataParams + 116), 16);

	printLogMem("Params ", (int) utilitySavedataParams, _lw((int) utilitySavedataParams + 0));
}
#endif

void syscallLog(const SyscallInfo *syscallInfo, const u32 *parameters, u64 result, u32 ra, const char *prefix) {
	char buffer[300];
	char *s = buffer;
	int i, j, k;
	int length;
	int lineStart;

	// Don't log our own sceIoWrites.
	if (syscallInfo->nid == 0x42EC03AC && parameters[0] == commonInfo->logFd) {
		return;
	}

	if (logTimestamp) {
		pspTime time;
		if (sceRtcGetCurrentClockLocalTime(&time) == 0) {
			s = appendInt(s, time.hour, 2);
			*s++ = ':';
			s = appendInt(s, time.minutes, 2);
			*s++ = ':';
			s = appendInt(s, time.seconds, 2);
			*s++ = '.';
			s = appendInt(s, time.microseconds, 6);
			*s++ = ' ';
		}
	}

	if (logThreadName) {
		SceKernelThreadInfo currentThreadInfo;
		currentThreadInfo.size = sizeof(currentThreadInfo);
		currentThreadInfo.name[0] = '\0';
		sceKernelReferThreadStatus(0, &currentThreadInfo);

		s = append(s, currentThreadInfo.name);
		*s++ = ' ';
		*s++ = '-';
		*s++ = ' ';
	}

	if (logRa) {
		s = appendHex(s, ra, 0);
		*s++ = ' ';
	}

	s = append(s, prefix);
	s = append(s, syscallInfo->name);
	int types = syscallInfo->paramTypes;
	for (i = 0; i < syscallInfo->numParams; i++, types >>= 4) {
		if (i > 0) {
			*s++ = ',';
		}
		*s++ = ' ';
		int parameter = parameters[i];
		switch (types & 0xF) {
			case TYPE_HEX32:
				s = appendHex(s, parameter, 0);
				break;
			case TYPE_INT32:
				s = appendInt(s, parameter, 0);
				break;
			case TYPE_STRING:
				s = appendHex(s, parameter, 8);
				if (parameter != 0) {
					*s++ = '(';
					*s++ = '\'';
					s = append(s, (char *) parameter);
					*s++ = '\'';
					*s++ = ')';
				}
				break;
			case TYPE_POINTER32:
				s = appendHex(s, parameter, 8);
				if (parameter != 0) {
					*s++ = '(';
					s = appendHex(s, _lw(parameter), 0);
					*s++ = ')';
				}
				break;
			case TYPE_POINTER64:
				s = appendHex(s, parameter, 8);
				if (parameter != 0) {
					*s++ = '(';
					s = appendHex(s, _lw(parameter), 8);
					*s++ = ' ';
					s = appendHex(s, _lw(parameter + 4), 8);
					*s++ = ')';
				}
				break;
			case TYPE_VARSTRUCT:
				s = appendHex(s, parameter, 8);
				if (parameter != 0) {
					*s++ = ':';
					*s++ = '\n';
					writeLog(buffer, s - buffer);
					s = buffer;
					length = _lw(parameter);
					lineStart = 0;
					for (j = 0; j < length; j += 4) {
						if (j > 0) {
							if ((j % 16) == 0) {
								s = append(s, "  >");
								for (k = lineStart; k < j; k++) {
									char c = _lb(parameter + k);
									if (c < ' ' || c > '~') {
										c = '.';
									}
									*s++ = c;
								}
								s = append(s, "<\n");
								writeLog(buffer, s - buffer);
								s = buffer;
								lineStart = j;
							} else {
								*s++ = ',';
								*s++ = ' ';
							}
						}
						s = appendHex(s, _lw(parameter + j), 8);
					}
				}
				break;
			case TYPE_FIXSTRUCT:
				s = appendHex(s, parameter, 8);
				if (parameter != 0) {
					*s++ = ':';
					*s++ = '\n';
					writeLog(buffer, s - buffer);
					s = buffer;
					length = FIXSTRUCT_SIZE;
					lineStart = 0;
					for (j = 0; j < length; j += 4) {
						if (j > 0) {
							if ((j % 16) == 0) {
								s = append(s, "  >");
								for (k = lineStart; k < j; k++) {
									char c = _lb(parameter + k);
									if (c < ' ' || c > '~') {
										c = '.';
									}
									*s++ = c;
								}
								s = append(s, "<\n");
								writeLog(buffer, s - buffer);
								s = buffer;
								lineStart = j;
							} else {
								*s++ = ',';
								*s++ = ' ';
							}
						}
						s = appendHex(s, _lw(parameter + j), 8);
					}
				}
				break;
		}
	}
	*s++ = ' ';
	*s++ = '=';
	*s++ = ' ';
	s = appendHex(s, (int) result, 0);

	#if DEBUG_MUTEX
	s = mutexLog(s, syscallInfo, parameters, result);
	#endif

	*s++ = '\n';
	writeLog(buffer, s - buffer);

	#if DEBUG_UTILITY_SAVEDATA
	utilitySavedataLog(buffer, syscallInfo, parameters[0]);
	#endif
}
