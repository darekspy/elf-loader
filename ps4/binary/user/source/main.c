#define _XOPEN_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <ps4/memory.h>
#include <ps4/util.h>
#include <ps4/kernel.h>

#include <kernel.h>

#include <elfloader.h>

#include "main.h"

/* Constants */

#define ELF_SERVER_IO_PORT 5052
#define ELF_SERVER_USER_PORT 5053
#define ELF_SERVER_KERNEL_PORT 5054
#define ELF_SERVER_KERNEL_PROCESS_PORT 5055 // untested (does not crash ...)
#define ELF_SERVER_RETRY 20
#define ELF_SERVER_TIMEOUT 1
#define ELF_SERVER_BACKLOG 10

typedef int (*ElfRunner)(Elf *elf);

typedef struct ElfServerArgument
{
	volatile int *run;
	pthread_t thread;
	ElfRunner elfRunner;
	int descriptor;
	int port;
}
ElfServerArgument;

/* Globals */

FILE *__stdinp;
FILE *__stdoutp;
FILE *__stderrp;
int __isthreaded;

/* Elf util */

Elf *elfCreateFromSocket(int client)
{
	Elf *elf;
	size_t s;

	void *m = ps4UtilFileAllocateFromDescriptorUnsized(client, &s);
	if(m == NULL)
		return NULL;

	elf = elfCreate(m, s);
	if(elf != NULL && !elfLoaderIsLoadable(elf))
	{
		elfDestroyAndFree(elf);
		elf = NULL;
	}

	return elf;
}

/* IO and elf servers */

void *elfLoaderServerIo(void *arg)
{
	ElfServerArgument *a = (ElfServerArgument *)arg;
	int client;

	if((a->descriptor = ps4UtilServerCreateEx(a->port, ELF_SERVER_BACKLOG, ELF_SERVER_RETRY, ELF_SERVER_TIMEOUT)) < 0)
	{
		*a->run = -1;
		return NULL;
	}

	if(a->descriptor >= 0 && a->descriptor < 3)
	{
		// something is wrong and we get a std io fd -> hard-fix with dummies
		ps4UtilStandardIoRedirectPlain(-1);
		a->descriptor = ps4UtilServerCreateEx(a->port, ELF_SERVER_BACKLOG, ELF_SERVER_RETRY, ELF_SERVER_TIMEOUT);
		if(a->descriptor >= 0 && a->descriptor < 3)
		{
			*a->run = -1;
			return NULL;
		}
	}

	*a->run = 1;
	while(*a->run == 1)
	{
		client = accept(a->descriptor, NULL, NULL);
		if(client < 0)
			continue;
		ps4UtilStandardIoRedirectPlain(client);
		close(client);
	}

	return NULL;
}

void *elfLoaderServerElf(void *arg)
{
	ElfServerArgument *a = (ElfServerArgument *)arg;
	int client;
	Elf *elf;

 	if((a->descriptor = ps4UtilServerCreateEx(a->port, ELF_SERVER_BACKLOG, ELF_SERVER_RETRY, ELF_SERVER_TIMEOUT)) < 0)
		return NULL;

	while(*a->run == 1)
	{
		client = accept(a->descriptor, NULL, NULL);
		if(client < 0)
			continue;
		elf = elfCreateFromSocket(client);
		close(client);
		if(elf == NULL)
			break;
		a->elfRunner(elf);
	}
	*a->run = 0;

	return NULL;
}

/* Userland elf runner */

void *elfLoaderUserMain(void *arg)
{
	ElfRunUserArgument *argument = (ElfRunUserArgument *)arg;
	char *elfName = "elf";
	char *elfArgv[3] = { elfName, NULL, NULL }; // double null term for envp
	int elfArgc = 1;
	int r;

	if(argument == NULL)
		return NULL;

	r = argument->main(elfArgc, elfArgv);
	ps4MemoryProtectedDestroy(argument->memory);
	//ps4MemoryDestroy(argument->memory);
	free(argument);
	printf("return (user): %i\n", r);

	return NULL;
}

int elfLoaderUserRun(Elf *elf)
{
	pthread_t thread;
	ElfRunUserArgument *argument;
	int r;

	if(elf == NULL)
		return -1;

 	argument = (ElfRunUserArgument *)malloc(sizeof(ElfRunUserArgument));
	if(argument ==  NULL)
	{
		elfDestroyAndFree(elf);
		return -1;
	}

	if(ps4MemoryProtectedCreate(&argument->memory, elfMemorySize(elf)) != 0)
	//if(ps4MemoryCreate(&argument->memory, elfMemorySize(elf)) != PS4_OK)
	{
		free(argument);
		elfDestroyAndFree(elf);
		return -1;
	}

	argument->main = NULL;
	r = elfLoaderLoad(elf, ps4MemoryProtectedGetWritableAddress(argument->memory), ps4MemoryProtectedGetExecutableAddress(argument->memory));
	//r = elfLoaderLoad(elf, ps4MemoryGetAddress(argument->memory), ps4MemoryGetAddress(argument->memory));
	if(r == ELF_LOADER_RETURN_OK)
		argument->main = (ElfMain)((uint8_t *)ps4MemoryProtectedGetExecutableAddress(argument->memory) + elfEntry(elf));
		//argument->main = (ElfMain)((uint8_t *)ps4MemoryGetAddress(argument->memory) + elfEntry(elf));
	elfDestroyAndFree(elf); // we don't need the "file" anymore

	if(argument->main != NULL)
		pthread_create(&thread, NULL, elfLoaderUserMain, argument);
	else
	{
		ps4MemoryProtectedDestroy(argument->memory);
		//ps4MemoryDestroy(argument->memory);
		free(argument);
		return -1;
	}

	return PS4_OK;
}

/* Kernel & process elf runner */
//FIXME: checks

void *elfLoaderKernelMain(void *arg)
{
	int p;
	int64_t r, ret;
	ElfRunKernelArgument *ka = (ElfRunKernelArgument *)arg;
	ps4KernelMemoryCopy(&ka->isProcess, &p, sizeof(int));
	ret = 0;
	r = ps4KernelExecute((void *)elfLoaderKernMain, ka, &ret, NULL);
	if(p == 0)
		printf("return (kernel): %i %"PRId64"\n", r, ret);
	else
		printf("return (kernel process): %i %"PRId64"\n", r, ret);
	return NULL;
}

int elfLoaderKernelRunEx(Elf *elf, int asProcess)
{
	pthread_t thread;
	ElfRunKernelArgument ua;
	ElfRunKernelArgument *ka;

	if(elf == NULL)
		return -1;

	ka = ps4KernelMemoryMalloc(sizeof(ElfRunKernelArgument));
	ua.isProcess = asProcess;
	ua.size = elfGetSize(elf);
	ua.data = ps4KernelMemoryMalloc(ua.size);
	ps4KernelMemoryCopy(elfGetData(elf), ua.data, ua.size);
	ps4KernelMemoryCopy(&ua, ka, sizeof(ElfRunKernelArgument));

	elfDestroyAndFree(elf); // we dispose of non-kernel data and rebuild and clean the elf in kernel

	pthread_create(&thread, NULL, elfLoaderKernelMain, ka);
	return PS4_OK;
}

int elfLoaderKernelRun(Elf *elf)
{
	return elfLoaderKernelRunEx(elf, 0);
}

int elfLoaderKernelProcessRun(Elf *elf)
{
	return elfLoaderKernelRunEx(elf, 1);
}

/* Setup and run threads */

int main(int argc, char **argv)
{
	volatile int run = 0;
	ElfServerArgument io, user, kernel, kernelProcess;

	int libc;
	FILE **__stdinp_address;
	FILE **__stdoutp_address;
	FILE **__stderrp_address;
	int *__isthreaded_address;
	struct sigaction sa;

	// resolve globals needed for standard io
 	libc = sceKernelLoadStartModule("libSceLibcInternal.sprx", 0, NULL, 0, 0, 0);
	sceKernelDlsym(libc, "__stdinp", (void **)&__stdinp_address);
	sceKernelDlsym(libc, "__stdoutp", (void **)&__stdoutp_address);
	sceKernelDlsym(libc, "__stderrp", (void **)&__stderrp_address);
	sceKernelDlsym(libc, "__isthreaded", (void **)&__isthreaded_address);
	__stdinp = *__stdinp_address;
	__stdoutp = *__stdoutp_address;
	__stderrp = *__stderrp_address;
	__isthreaded = *__isthreaded_address;

	// Suppress sigpipe
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigaction(SIGPIPE, &sa, 0);

	// Change standard io buffering
	setvbuf(stdin, NULL, _IOLBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	// Setup of server logic(s)
	io.run = user.run = kernel.run = kernelProcess.run = &run;
	io.port = ELF_SERVER_IO_PORT;
	io.elfRunner = NULL;
	user.port = ELF_SERVER_USER_PORT;
	user.elfRunner = elfLoaderUserRun;
	kernel.port = ELF_SERVER_KERNEL_PORT;
	kernel.elfRunner = elfLoaderKernelRun;
	kernelProcess.port = ELF_SERVER_KERNEL_PROCESS_PORT;
	kernelProcess.elfRunner = elfLoaderKernelProcessRun;

	//FIXME: check pthread_creates
	// Start handling threads
 	// "io" will set run once its set up to ensure it gets fds 0,1 and 2 or fail using run
	pthread_create(&io.thread, NULL, elfLoaderServerIo, &io);
	while(run == 0)
		sleep(0);
	if(run == -1)
		return EXIT_FAILURE;

	pthread_create(&user.thread, NULL, elfLoaderServerElf, &user);
	pthread_create(&kernel.thread, NULL, elfLoaderServerElf, &kernel);
	pthread_create(&kernelProcess.thread, NULL, elfLoaderServerElf, &kernelProcess);

	// If you like to stop the threads, best just close/reopen the browser
	// Otherwise send non-elf
	while(run == 1)
		sleep(1);

	shutdown(io.descriptor, SHUT_RDWR);
	shutdown(user.descriptor, SHUT_RDWR);
	shutdown(kernel.descriptor, SHUT_RDWR);
	shutdown(kernelProcess.descriptor, SHUT_RDWR);
	close(io.descriptor);
	close(user.descriptor);
	close(kernel.descriptor);
	close(kernelProcess.descriptor);

	// This does not imply that the started elfs ended
	// Kernel stuff will continue to run
	// User stuff will end on browser close
	pthread_join(io.thread, NULL);
	pthread_join(user.thread, NULL);
	pthread_join(kernel.thread, NULL);
	pthread_join(kernelProcess.thread, NULL);

	return 0;
}
