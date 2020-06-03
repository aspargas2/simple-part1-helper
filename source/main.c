#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <errno.h>
#include <3ds.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

#define ADDRESS         "192.168.1.4"
#define PORT            7850

int sockfd;
static u32 *SOC_buffer = NULL;

static Handle friendsEvent;
static Handle s_terminate;

void HandleFriendNotification(NotificationEvent *event)
{
	switch (event->type)
	{
	
	// triggers whenever a friend we've added adds us back
	case FRIEND_REGISTERED_USER:
	{
		int ret;
		printf("registered %lx as a friend\n", event->key.principalId);
		u64 lfcs = event->key.localFriendCode & 0xFFFFFFFFFFLL;
		printf("lfcs: %0llx\n", lfcs);
		printf("lfc: %0llx\n", event->key.localFriendCode);
		u64 friendCode;
		FRD_PrincipalIdToFriendCode(event->key.principalId, &friendCode);
		
		// wait a maximum of 10 seconds for the socket to be ready to send data
		fd_set fdset;
		FD_ZERO(&fdset);
		FD_SET(sockfd, &fdset);
		struct timeval timeout;
		timeout.tv_sec = 10;
		timeout.tv_usec = 0;
		ret = select(sockfd+1, NULL, &fdset, NULL, &timeout);
		printf("send select returned %d\n", ret);
		if (ret < 0)
			printf("send select failed with %d %s\n", errno, strerror(errno));
		else if (ret == 0)
			printf("send select timed out\n");
		else {
			if (send(sockfd, &friendCode, 8, 0) < 0)
				printf("Send failed with %d %s\n", errno, strerror(errno));
			if (send(sockfd, &lfcs, 8, 0) < 0)
				printf("Send failed with %d %s\n", errno, strerror(errno));
		}
		
		// remove the friend once we've gotten its LFCS
		if (R_FAILED(ret = FRD_RemoveFriend(event->key.principalId, event->key.localFriendCode))) {
			printf("Removing friend %012llu failed with 0x%08X\n\n", friendCode, (unsigned int)ret);
			return;
		}
		printf("Successfully removed friend %012llu\n\n", friendCode);
	}
	break;

	default:
		printf("notification %d recieved for %lx while FRIEND_REGISTERED_USER is %d\n", event->type, event->key.principalId, FRIEND_REGISTERED_USER);
		break;
	}
}

void FriendNotificationHandlerThread(void *n)
{
	svcCreateEvent(&friendsEvent, RESET_ONESHOT);

	FRD_AttachToEventNotification(friendsEvent);
	s32 out;
	Handle frd_handles[] = {friendsEvent ,s_terminate};
	bool run = true;

	NotificationEvent events[10];
	while (run)
	{
		svcWaitSynchronizationN(&out, frd_handles, 2, false, U64_MAX);
		switch (out)
		{
			case 0:
			{
				size_t size = 0;
				do
				{
					FRD_GetEventNotification(events, 10, (u32 *)&size);


					for (u64 i = 0; i < size; ++i)
					{
						HandleFriendNotification(events + i);
					}

				} while (size != 0);
				break;
			}
			case 1:
				run = false;
				break;
		}
	}
	svcClearEvent(s_terminate);

}

int main()
{
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	
	int ret;
	bool failed = false;
	struct sockaddr_in serv_addr;
	memset (&serv_addr, 0, sizeof(serv_addr));
	
	// allocate aligned buffer for soc:u service
	SOC_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
	if(SOC_buffer == NULL) {
		failed = true;
		printf("memalign: failed to allocate\n");
	}

	// initialize soc:u service
	if (!failed && R_FAILED(ret = socInit(SOC_buffer, SOC_BUFFERSIZE))) {
		failed = true;
		printf("socInit: 0x%08X\n", (unsigned int)ret);
	}
	
	// initialize frd:u service
	if (!failed && R_FAILED(ret = frdInit())) {
		failed = true;
		printf("frdInit: 0x%08X\n", (unsigned int)ret);
	}
	
	// create event that will trigger termination of the new thread
	svcCreateEvent(&s_terminate, RESET_ONESHOT);
	
	// create a new thread to handle gathering LFCSs when friends add the system back
	Thread thread = threadCreate(FriendNotificationHandlerThread, NULL, 4096 , 0x24, 0, true);

	// create a new socket which will handle all communication with the server
	if (!failed && ((sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0)) {
		failed = true;
		printf("socket returned %d\n", sockfd);
	}
	
	if (!failed) {
		serv_addr.sin_family = AF_INET; // IPv4
		serv_addr.sin_port = htons(PORT);

		if (inet_pton(AF_INET, ADDRESS, &serv_addr.sin_addr) <= 0) {
			failed = true;
			printf("Invalid address / Address not supported\n");
		}
	}
	
	// try connecting to the server
	if (!failed && ((ret = connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr))) < 0)) {
		failed = true;
		printf("Connection to %s failed with %d %s\n", ADDRESS, errno, strerror(errno));
		// retry connection 9 more times at 10 second intervals, then give up
		for (int i = 2; i <= 10; i++) {
			printf("Retrying in 10 seconds (%d/10)\n", i);
			svcSleepThread(10000 * 1e6);
			printf("Retrying...\n");
			if ((ret = connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr))) >= 0) {
				failed = false;
				break;
			}
			printf("Connection to %s failed with %d %s\n", ADDRESS, errno, strerror(errno));
		}
		if (failed) close(sockfd);
	}

	if (!failed)
		printf("successfully connected to %s on port %d, entering loop!\n", ADDRESS, PORT);

	// Main loop
	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		u64 fc;
		u32 princId;
		
		if (!failed) {
			fd_set fdset;
			FD_ZERO(&fdset);
			FD_SET(sockfd, &fdset);
			struct timeval timeout;
			timeout.tv_sec = 10;
			timeout.tv_usec = 0;
			ret = select(sockfd+1, &fdset, NULL, NULL, &timeout);
			if (ret < 0) {
				printf("Recv select failed with %d %s\n", errno, strerror(errno));
				failed = true;
				continue;
			} if (ret == 0) {
				//printf("nothing to recv\n");
				// don't try to read from the socket unless there's something there to read
				// letting this thread sit on the recv call waiting for incoming data will interfere with send calls on the other thread
				continue;
			} 
			
			// assume any incoming data is a u64 friend code
			if ((ret = recv(sockfd, &fc, 8, 0)) <= 0) {
				printf("Recv failed with %d %s\n", errno, strerror(errno));
				failed = true;
				continue;
			}
			printf("Read %d bytes: %012llu\n", ret, fc);
		
			if (R_FAILED(ret = FRD_FriendCodeToPrincipalId(fc, &princId))) {
				printf("Getting principal id failed with 0x%08X\n", (unsigned int)ret);
				continue;
			}

			// add the recieved friend code as an online friend and wait a maximum of 5 seconds for it to finish adding
			Handle addFriendEvent;
			svcCreateEvent(&addFriendEvent, RESET_STICKY);
			if (R_FAILED(ret = FRD_AddFriendOnline(addFriendEvent, princId))) {
				printf("Adding friend failed with 0x%08X\n", (unsigned int)ret);
				continue;
			}
			if (!failed && R_FAILED(ret = svcWaitSynchronization(addFriendEvent,(5 * 1e9)))) {
				printf("Waiting on friend add event failed with 0x%08X\n", (unsigned int)ret);
				continue;
			}
		}

		// there's currently no good way to successfully exit the app, the user just has to terminate the server and let this app error out
		u32 kHeld = hidKeysHeld();
		if (kHeld & KEY_START)
			break;
	}

	if (!failed) {
		printf("waiting on close...\n");
		close(sockfd);
	}
	
	svcSignalEvent(s_terminate);
	threadJoin(thread, 300 * 1e9);
	svcCloseHandle(s_terminate);
	
	socExit();
	gfxExit();
	return 0;
}
