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
	case FRIEND_REGISTERED_USER:
	{
		int ret;
		printf("registered %lx as a friend\n", event->key.principalId);
		u64 lfcs = event->key.localFriendCode & 0xFFFFFFFFFFLL;
		printf("lfcs: %0llx\n", lfcs);
		printf("lfc: %0llx\n", event->key.localFriendCode);
		u64 friendCode;
		FRD_PrincipalIdToFriendCode(event->key.principalId, &friendCode);
		//svcSleepThread(500 * 1e6);
		fd_set fdset;
		FD_ZERO(&fdset);
		FD_SET(sockfd, &fdset);
		struct timeval timeout;
		timeout.tv_sec = 10;
		timeout.tv_usec = 0;
		ret = select(sockfd+1, NULL, &fdset, NULL, &timeout);
		printf("send select returned %d\n", ret);
		if (ret < 0)
			printf("Select failed with %d %s\n", errno, strerror(errno));
		//printf("at sends\n");
		if (send(sockfd, &friendCode, 8, 0) < 0)
			printf("Send failed with %d %s\n", errno, strerror(errno));
		if (send(sockfd, &lfcs, 8, 0) < 0)
			printf("Send failed with %d %s\n", errno, strerror(errno));
		//printf("past sends\n");
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
	
	// allocate buffer for SOC service
	SOC_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);

	if(SOC_buffer == NULL) {
		failed = true;
		printf("memalign: failed to allocate\n");
	}

	// Now intialise soc:u service
	if (!failed && R_FAILED(ret = socInit(SOC_buffer, SOC_BUFFERSIZE))) {
		failed = true;
		printf("socInit: 0x%08X\n", (unsigned int)ret);
	}
	
	if (!failed && R_FAILED(ret = frdInit())) {
		failed = true;
		printf("frdInit: 0x%08X\n", (unsigned int)ret);
	}
	
	svcCreateEvent(&s_terminate, RESET_ONESHOT);
	Thread thread = threadCreate(FriendNotificationHandlerThread, NULL, 4096 , 0x24, 0, true);

	if (!failed && ((sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0)) {
		failed = true;
		printf("socket returned %d\n", sockfd);
	}
	
	if (!failed) {
		serv_addr.sin_family = AF_INET; 
		serv_addr.sin_port = htons(PORT);

		if (inet_pton(AF_INET, ADDRESS, &serv_addr.sin_addr) <= 0) {
			failed = true;
			printf("Invalid address / Address not supported\n");
		}
	}
	
	/*if (!failed) {
		//fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) & ~O_NONBLOCK);
		
		if ((ret = bind (sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)))) {
			close(sockfd);
			failed = true;
			printf("bind: %d %s\n", errno, strerror(errno));
		}
	}*/
	
	//svcSleepThread(500 * 1e6);
	
	if (!failed && ((ret = connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr))) < 0)) {
		failed = true;
		printf("Connection to %s failed with %d %s\n", ADDRESS, errno, strerror(errno));
		for (int i = 2; i <= 10; i++) {
			printf("Retrying in 10 seconds (%d/10)\n", i);
			svcSleepThread(10000 * 1e6);
			printf("Retrying...\n");
			//close(sockfd);
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
			//printf("Recv select returned %d\n", ret);
			if (ret < 0) {
				printf("Recv select failed with %d %s\n", errno, strerror(errno));
				failed = true;
				continue;
			}
			if (ret == 0) {
				//printf("nothing to recv\n");
				continue;
			}
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

		u32 kHeld = hidKeysHeld();
		if (kHeld & KEY_START)
			break; // break in order to return to hbmenu
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
