#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <errno.h>

///////////////////////////////////////////////////////////////////////////////////////////
// Utils

#define bool int
#define true 1
#define false 0

#define define_vector(type) \
typedef struct vector_##type \
{ \
    int size; \
    int allocation; \
    type* a; \
} vector_##type; \
\
void push_back_##type(vector_##type* vec, type value) \
{ \
    if (vec->size == vec->allocation) \
    { \
        vec->allocation *= 2; \
        vec->a = realloc(vec->a, vec->allocation * sizeof(type)); \
    } \
    vec->a[vec->size++] = value; \
} \
\
void free_vector_##type(vector_##type* vec) \
{ \
	if (vec->a) \
	{ \
		free(vec->a); \
	} \
	vec->a = NULL; \
} \

#define vector(type) \
({ \
    vector_##type vec; \
    vec.size = 0; \
    vec.allocation = 1; \
    vec.a = (type*)calloc(1, sizeof(type)); \
    vec; \
})

#define init_vector(type, _size, value) \
({ \
    vector_##type vec; \
    vec.size = _size; \
    if (!(_size & (_size - 1))) \
        vec.allocation = _size; \
    else \
        vec.allocation =  0x8000000000000000 >> (__builtin_clzll(_size) - 1); \
    vec.a = (type*)calloc(vec.allocation, sizeof(type)); \
    memset(vec.a, value, _size * sizeof(type)); \
    vec; \
})

define_vector(int)

// convert string to int
bool getInt(char* token, int* n)
{
    *n = 0;
    char c;

    // trim spaces and tabs
    for (; *token == ' ' || *token == '\t'; token++);

    do {
        c = *token++;

        if (c >= 48 && c <= 57)
        {
            *n = *n * 10 + (c - 48);
        }
        else if (c == '\n' || c == ' ' || c == '\t' || c == '\0' || c == '\r')
        {
            break;
        }
        else
        {
            *n = -1;
            return false;
        }

    } while (1);

    return true;
}

// get next token, delimiter = space/tab
bool getNext(char** pname, char token[])
{
    int i = 0;

    // trim spaces and tabs
    for (; **pname == ' ' || **pname == '\t'; (*pname)++);

    // empty string provided
    if (**pname == '\0')
    {
        return false;
    }

    while (**pname != ' ' && **pname != '\t' && **pname != '\0')
    {
        token[i++] = **pname;
        (*pname)++;
    }

    token[i] = '\0';
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////
// Semaphore

int semId;
struct sembuf pop = { .sem_num = 0, .sem_op = -1, .sem_flg = 0 };
struct sembuf vop = { .sem_num = 0, .sem_op = 1, .sem_flg = 0 };

#define P(s) semop(s, &(pop), 1)
#define V(s) semop(s, &(vop), 1)

void createAndInitSemaphore()
{
	// create 1 semaphore
	if ((semId = semget(IPC_PRIVATE, 1, 0660 | IPC_CREAT)) == -1)
	{
		perror("semget");
		exit(1);
	}

	union semun
    {
        int val;
        struct semid_ds *buf;
        unsigned short array[1];
    } sem_attr;

	sem_attr.val = 1; // semaphore initial value
	if (semctl(semId, 0, SETVAL, sem_attr) == -1)
	{
        perror(" semctl SETVAL ");
		exit(1);
    }
}

void removeSemapohore()
{
	if (semctl(semId, 0, IPC_RMID) == -1)
	{
        perror("semctl IPC_RMID");
		exit(1);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////
// Data Structures

typedef struct client
{
	bool active;
	int sockfd;
	int uid;
	int* groups; // ids of the groups the client is in
} client;

enum groupstatus
{
	offline,
	requested,
	active
};

typedef struct adminreq
{
	int client;
	vector_int admins;
	vector_int replies;
} adminreq;

define_vector(adminreq)

typedef struct group
{
	int gid;
	enum groupstatus status;
	bool broadcastOnly;
	int* admins;
	int* members;
	vector_int requested_members;
	vector_int replies;
	vector_adminreq adminreqs;
} group;

///////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////
// Members

client* clients;
group* groups;
int numconnections = 0;
int numclients = 10;
int numgroups = 10;
int maxgroupsize = 5;
int sockfd;
fd_set master;
int fdmax;

///////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////
// Helper functions
void cleanup()
{
	// free groups
	for (int i = 0; i < numgroups; i++)
	{
		free_vector_int(&groups[i].requested_members);
		free_vector_int(&groups[i].replies);
		free_vector_adminreq(&groups[i].adminreqs);
	}
}

int initGroup(group* g, int admin, int members[])
{
	g->gid = rand() % 89999 + 10000;
	g->admins[0] = admin;
	for (int i = 0; i < maxgroupsize - 1; i++)
	{
		g->members[i] = members[i];
	}

	g->members[maxgroupsize - 1] = admin;
	g->adminreqs = vector(adminreq);
	g->status = active;
	return g->gid;
}

int initGroupReq(group* g, int admin, vector_int* members)
{
	g->gid = rand() % 89999 + 10000;
	g->admins[0] = admin;

	g->requested_members = vector(int);
	for (int i = 1; i < members->size; i++)
	{
		push_back_int(&(g->requested_members), members->a[i]);
	}

	g->replies = init_vector(int, g->requested_members.size, -1);
	g->adminreqs = vector(adminreq);
	g->status = requested;
	return g->gid;
}

bool isMember(group* g, int uid)
{
	for (int i = 0; i < numclients; i++)
	{
		if (g->members[i] != -1 && uid == g->members[i])
		{
			return true;
		}
	}

	return false;
}

bool isAdmin(group* g, int uid)
{
	for (int i = 0; i < numclients; i++)
	{
		if (g->admins[i] != -1 && uid == g->admins[i])
		{
			return true;
		}
	}

	return false;
}

int addMember(group* g, int admin, int uid)
{
	if (!isAdmin(g, admin))
	{
		// no privileges
		return -3;
	}

	if (isMember(g, uid))
	{
		// client is already a member
		return -1;
	}
	
	for (int i = 0; i < numclients; i++)
	{
		if (g->members[i] == -1)
		{
			g->members[i] = uid;
			return 0;
		}
	}

	// group is full
	return -2;
}

int addAdmin(group* g, int admin, int uid)
{
	if (!isAdmin(g, admin))
	{
		// no privileges
		return -3;
	}

	if (!isMember(g, uid))
	{
		// client is not a member
		return -1;
	}

	if (isAdmin(g, uid))
	{
		// client is already an admin
		return -2;
	}
	
	for (int i = 0; i < numclients; i++)
	{
		if (g->admins[i] == -1)
		{
			g->admins[i] = uid;
			return 0;
		}
	}

	// group is full
	return -2;
}

int removeMember(group* g, int admin, int uid)
{
	if (!isAdmin(g, admin))
	{
		// no privileges
		return -3;
	}

	if (!isMember(g, uid))
	{
		// client is not a member
		return -1;
	}

	for (int i = 0; i < numclients; i++)
	{
		if (g->members[i] == uid)
		{
			g->members[i] = -1;
		}
	}

	for (int i = 0; i < numclients; i++)
	{
		if (g->admins[i] == uid)
		{
			g->admins[i] = -1;
		}
	}

	return 0;
}

int getGroupSize(group* g)
{
	int size = 0;
	for (int i = 0; i < maxgroupsize; i++)
	{
		if (g->members[i] != -1)
		{
			size++;
		}
	}

	return size;
}

int getGroupIndex(int gid)
{
	for (int i = 0; i < numgroups; i++)
	{
		if (groups[i].status == active && groups[i].gid == gid)
		{
			return i;
		}
	}

	return -1;
}

int makeBroadcastOnly(group* g, int admin)
{
	if (!isAdmin(g, admin))
	{
		// client is not an admin
		return -1;
	}

	g->broadcastOnly = true;
	return 0;
}

void ActivateGroup(group* g)
{
	int k = 0;
	for (int i = 0; i < g->replies.size; i++)
	{
		if (g->replies.a[i] == 1)
		{
			g->members[k++] = g->requested_members.a[i];
		}
	}
	g->members[k] = g->admins[0];
	g->status = active;
	free_vector_int(&g->requested_members);
	free_vector_int(&g->replies);
}

// function to handle ^C*
void sigCHandler() 
{ 
	cleanup();
	exit(0);
} 

// function to handle ^Z
void sigZhandler() 
{
	cleanup();
	exit(0);
}

int getMaxFd()
{
	int fdmax = 0;
	for (int j = 0; j < numclients; j++)
	{
		fdmax = clients[j].sockfd > fdmax ? clients[j].sockfd : fdmax;
	}
	fdmax = sockfd > fdmax ? sockfd : fdmax;

	return fdmax;
}

int getIndexForNewGroup()
{
	int i;
	for (i = 0; i < numgroups; i++)
	{
		if (groups[i].status == offline)
		{
			return i;
		}
	}

	return -1;
}

int getIndexForNewGroupInClient(client* c)
{
	for (int i = 0; i < numgroups; i++)
	{
		if (c->groups[i] == -1)
		{
			return i;
		}
	}

	return -1;
}

int getClientIndex(int cid)
{
	for (int i = 0; i < numclients; i++)
	{
		if (clients[i].uid != -1 && clients[i].uid == cid)
		{
			return i;
		}
	}

	return -1;
}

int getIndexForNewClient()
{
	int i;
	for (i = 0; i < numclients; i++)
	{
		if (clients[i].active == false)
		{
			return i;
		}
	}

	return -1;
}

bool writeToClient(int i, char buffer[]);

void removeClient(int i)
{
	// remove the fd from set
	FD_CLR(clients[i].sockfd, &master);

	clients[i].sockfd = -1;

	// find max fd value
	fdmax = getMaxFd();

	// mark the slot empty
	clients[i].active = false;
	
	// decrement number of active clients
	numconnections--;

	// remove client from all groups
	for (int j = 0; j < numgroups; j++)
	{
		int gid = clients[i].groups[j];
		if (gid == -1)
		{
			continue;
		}

		int g = getGroupIndex(gid);
		
		for (int k = 0; k < maxgroupsize; k++)
		{
			int client = groups[g].members[k];
			if (client == -1)
			{
				continue;
			}

			if (client == i)
			{
				groups[g].members[k] = -1;
			}
			else
			{
				char out[256];
				sprintf(out, "client %d left the group %d.\n", clients[i].uid, groups[g].gid);
				writeToClient(client, out);
			}
		}

		for (int k = 0; k < maxgroupsize; k++)
		{
			int client = groups[g].admins[k];
			if (client != -1 && client == i)
			{
				groups[g].admins[k] = -1;
			}
		}

		bool allAdminsInactive = true;
		for (int k = 0; k < maxgroupsize; k++)
		{
			int client = groups[g].admins[k];
			if (client != -1)
			{
				allAdminsInactive = false;
			}
		}

		if (allAdminsInactive)
		{
			// deactivate the group
			char out[256];
			sprintf(out, "group %d has been deactivated.", gid);
			for (int k = 0; k < maxgroupsize; k++)
			{
				int client = groups[g].members[k];
				if (client == -1)
				{
					continue;
				}

				for (int l = 0; l < maxgroupsize; l++)
				{
					if (clients[client].groups[l] == gid)
					{
						clients[client].groups[l] = -1;
					}
				}

				writeToClient(client, out);
				groups[g].members[k] = -1;
			}
			free_vector_adminreq(&groups[g].adminreqs);
			groups[g].status = offline;
		}

		clients[i].groups[j] = -1;
	}
}

// sends buffer of size 256 chars
bool writeToClient(int i, char buffer[])
{
	// printf("sending: %s\n", buffer);
	if (write(clients[i].sockfd, buffer, strlen(buffer)) == -1)
	{
		removeClient(i);
		return false;
	}

	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////
// Client command functions

/* function to processs messages from clients*/
void sendMsg(int i, char buffer[])
{
	//WRITE YOUR CODE HERE
	char out[256];
	sprintf(out, "client %d: ", clients[i].uid);

	char dest[256];
	if (!getNext(&buffer, dest))
	{
		// handle error
		strcpy(out, "server: client id not provided");
		writeToClient(i, out);
		return;
	}

	int destclient;
	if (!getInt(dest, &destclient))
	{
		// handle error
		strcpy(out, "server: invalid client id");
		writeToClient(i, out);
		return;
	}

	strcat(out, buffer);
	for (int j = 0; j < numclients; j++)
	{
		if (i != j && clients[j].active && clients[j].uid == destclient)
		{
			//write(clients[j].sockfd, out, sizeof(out));
			if (!writeToClient(j, out))
			{
				// client disconnected
				sprintf(out, "server: client %d disconnected", clients[j].uid);
				writeToClient(i, out);
			}
			
			return;
		}
	}

	// handle client not found
	sprintf(out, "server: client %d disconnected", destclient);
	writeToClient(i, out);
}

void broadcast(int i, char buffer[])
{
	char out[256];
	sprintf(out, "client %d broadcast: ", clients[i].uid);
	strcat(out, buffer);
	for (int j = 0; j < numclients; j++)
	{
		if (clients[j].active)
		{
			writeToClient(j, out);
		}
	}
}

void makeGroup(int i, char buffer[])
{
	// to store indices of clients[]
	int* members = (int*)calloc(maxgroupsize, sizeof(int));
	memset(members, -1, maxgroupsize * sizeof(int));

	char token[256];
	int j = 0;
	while (j < maxgroupsize - 1 && getNext(&buffer, token))
	{
		int clientid;
		if (!getInt(token, &clientid))
		{
			// invalid client id provided
			char out[256];
			sprintf(out, "server: invalid client id provided.");
			writeToClient(i, out);
			return;
		}

		int k;
		for (k = 0; k < numclients; k++)
		{
			if (clients[k].active && clients[k].uid == clientid)
			{
				break;
			}
		}

		if (k == numclients)
		{
			// client does not exist
			char out[256];
			sprintf(out, "client %d does not exist.", clientid);
			writeToClient(i, out);
			return;
		}

		members[j++] = k;
	}

	if (j == maxgroupsize - 1 && getNext(&buffer, token))
	{
		// max group size limit is 5
		char out[256];
		sprintf(out, "server: max group size limit is %d.", maxgroupsize);
		writeToClient(i, out);
		return;
	}

	int index = getIndexForNewGroup(numgroups, groups);
	if (index == -1)
	{
		// all groups are filled
		char out[256];
		sprintf(out, "server: all groups are filled.");
		writeToClient(i, out);
		return;
	}

	// create the group
	int gid = initGroup(&groups[index], i, members);

	// send responses
	char out[256];
	sprintf(out, "You have been added to group %d.", gid);
	for (int j = 0; j < maxgroupsize; j++)
	{
		if (groups[index].members[j] == -1)
		{
			continue;
		}

		int client = groups[index].members[j];

		// set gid in client
		int index = getIndexForNewGroupInClient(&clients[client]);
		if (index == -1)
		{
			// should never happen!
			// client already joined max number of groups
			char out[256];
			sprintf(out, "client %d already joined max number of groups.", client);
			writeToClient(i, out);
			continue;
		}
		clients[client].groups[index] = gid;

		writeToClient(client, out);
	}
	
	free(members);
}

void sendGroup(int i, char buffer[])
{
	char gid_s[256];
	if (!getNext(&buffer, gid_s))
	{
		// handle error
		char out[256];
		strcpy(out, "server: group id not provided");
		writeToClient(i, out);
		return;
	}

	int gid;
	if (!getInt(gid_s, &gid))
	{
		// handle error
		char out[256];
		strcpy(out, "server: invalid group id");
		writeToClient(i, out);
		return;
	}

	int group = getGroupIndex(gid);
	if (group == -1)
	{
		// group does not exist
		char out[256];
		strcpy(out, "server: group does not exist.");
		writeToClient(i, out);
		return;
	}

	if (!isMember(&groups[group], i))
	{
		// client is not a member
		char out[256];
		sprintf(out, "server: You are not a member of group %d.", gid);
		writeToClient(i, out);
		return;
	}

	// check if the group is broadcast only
	if (groups[group].broadcastOnly && !isAdmin(&groups[group], i))
	{
		// client is not a member
		char out[256];
		sprintf(out, "server: Only admins can send messages in a broadcast only group.");
		writeToClient(i, out);
		return;
	}

	char message[256];
	strcpy(message, buffer);

	char out[256];
	sprintf(out, "group %d: client %d: ", gid, clients[i].uid);
	strcat(out, message);
	for (int j = 0; j < maxgroupsize; j++)
	{
		int client = groups[group].members[j];
		if (client == -1)
		{
			continue;
		}
		
		writeToClient(client, out);
	}
}

void activeGroup(int i, char buffer[])
{
	char out[256];
	strcpy(out, "server: ");
	for (int j = 0; j < numgroups; j++)
	{
		if (clients[i].groups[j] == -1)
		{
			continue;
		}

		char gid[6];
		sprintf(gid, "%d", clients[i].groups[j]);
		strcat(out, gid);
		strcat(out, " ");
	}

	if (strcmp(out, "server: ") == 0)
	{
		strcat(out, "You are not in any groups.");
	}

	writeToClient(i, out);
}

void makeAdmin(int i, char buffer[])
{
	char gid_s[256];
	if (!getNext(&buffer, gid_s))
	{
		// handle error
		char out[256];
		strcpy(out, "server: group id not provided");
		writeToClient(i, out);
		return;
	}

	int gid;
	if (!getInt(gid_s, &gid))
	{
		// handle error
		char out[256];
		strcpy(out, "server: invalid group id");
		writeToClient(i, out);
		return;
	}

	int g = getGroupIndex(gid);
	if (g == -1)
	{
		// handle error
		char out[256];
		strcpy(out, "server: group does not exists.");
		writeToClient(i, out);
		return;
	}

	char cid_s[256];
	if (!getNext(&buffer, cid_s))
	{
		// handle error
		char out[256];
		strcpy(out, "server: client id not provided");
		writeToClient(i, out);
		return;
	}

	int cid;
	if (!getInt(cid_s, &cid))
	{
		// handle error
		char out[256];
		strcpy(out, "server: invalid client id");
		writeToClient(i, out);
		return;
	}

	int c = getClientIndex(cid);
	if (c == -1)
	{
		// handle error
		char out[256];
		strcpy(out, "server: client does not exist.");
		writeToClient(i, out);
		return;
	}

	int res = addAdmin(&groups[g], i, c);
	if (res == -3)
	{
		// no privileges
		char out[256];
		strcpy(out, "server: !!You are not an admin!!");
		writeToClient(i, out);
	}
	else if (res == -1)
	{
		// client is not a member
		char out[256];
		sprintf(out, "server: client %d is not a member of group %d.", cid, gid);
		writeToClient(i, out);
	}
	else if (res == -2)
	{
		// client is already an admin
		char out[256];
		sprintf(out, "server: client %d is already an admin of group %d.", cid, gid);
		writeToClient(i, out);
	}
	else
	{
		char out[256];
		sprintf(out, "server: You are now an admin of group %d.", gid);
		writeToClient(c, out);
	}
}

void addToGroup(int i, char buffer[])
{
	char gid_s[256];
	if (!getNext(&buffer, gid_s))
	{
		// handle error
		char out[256];
		strcpy(out, "server: group id not provided");
		writeToClient(i, out);
		return;
	}

	int gid;
	if (!getInt(gid_s, &gid))
	{
		// handle error
		char out[256];
		strcpy(out, "server: invalid group id");
		writeToClient(i, out);
		return;
	}

	int g = getGroupIndex(gid);
	if (g == -1)
	{
		// handle error
		char out[256];
		strcpy(out, "server: group does not exists.");
		writeToClient(i, out);
		return;
	}

	// to store indices of clients[]
	int* members = (int*)calloc(maxgroupsize, sizeof(int));
	memset(members, -1, maxgroupsize * sizeof(int));

	char token[256];
	int j = 0;
	int slotsLeft = maxgroupsize - getGroupSize(&groups[g]);
	while (j < slotsLeft && getNext(&buffer, token))
	{
		int clientid;
		if (!getInt(token, &clientid))
		{
			// invalid client id provided
			char out[256];
			sprintf(out, "server: invalid client id provided.");
			writeToClient(i, out);
			return;
		}

		int k;
		for (k = 0; k < numclients; k++)
		{
			if (clients[k].active && clients[k].uid == clientid)
			{
				break;
			}
		}

		if (k == numclients)
		{
			// client does not exist
			char out[256];
			sprintf(out, "client %d does not exist.", clientid);
			writeToClient(i, out);
			return;
		}

		members[j++] = k;
	}

	if (j == slotsLeft && getNext(&buffer, token))
	{
		// max group size limit is 5
		char out[256];
		sprintf(out, "server: max group size limit is %d, current group size: %d.", maxgroupsize, maxgroupsize - slotsLeft);
		writeToClient(i, out);
		return;
	}

	for (int j = 0; j < maxgroupsize; j++)
	{
		if (members[j] == -1)
		{
			continue;
		}

		int client = members[j];
		int res = addMember(&groups[g], i, client);
		if (res == -3)
		{
			// no privileges
			char out[256];
			sprintf(out, "server: !!You are not an admin!!");
			writeToClient(i, out);
			return;
		}
		else if (res == -1)
		{
			// already a member
			char out[256];
			sprintf(out, "server: client %d is already a member.", clients[client].uid);
			writeToClient(i, out);
			continue;
		}
		else if (res == -2)
		{
			// group is full -> should never come here!
			char out[256];
			sprintf(out, "server: group %d is full.", gid);
			writeToClient(i, out);
			return;
		}
		else
		{
			// send responses to the client added

			// set gid in client
			int index = getIndexForNewGroupInClient(&clients[client]);
			if (index == -1)
			{
				// should never happen!
				// client already joined max number of groups
				char out[256];
				sprintf(out, "client %d already joined max number of groups.", client);
				writeToClient(i, out);
				continue;
			}
			clients[client].groups[index] = gid;

			char out[256];
			sprintf(out, "You have been added to group %d.", gid);
			writeToClient(client, out);
		}
	}

	free(members);
}

void makeGroupBroadcast(int i, char buffer[])
{
	char gid_s[256];
	if (!getNext(&buffer, gid_s))
	{
		// handle error
		char out[256];
		strcpy(out, "server: group id not provided");
		writeToClient(i, out);
		return;
	}

	int gid;
	if (!getInt(gid_s, &gid))
	{
		// handle error
		char out[256];
		strcpy(out, "server: invalid group id");
		writeToClient(i, out);
		return;
	}

	int g = getGroupIndex(gid);
	if (g == -1)
	{
		// handle error
		char out[256];
		strcpy(out, "server: group does not exists.");
		writeToClient(i, out);
		return;
	}

	if (makeBroadcastOnly(&groups[g], i) == -1)
	{
		// client is not an admin
		char out[256];
		strcpy(out, "server: !!You are not an admin!!");
		writeToClient(i, out);
	}
	else
	{
		for (int j = 0; j < maxgroupsize; j++)
		{
			int client = groups[g].members[j];
			if (client == -1)
			{
				continue;
			}
			char out[256];
			sprintf(out, "server: group %d is now broadcast only.", gid);
			writeToClient(client, out);
		}
	}
}

void makeGroupReq(int i, char buffer[])
{
	// to store indices of clients[]
	vector_int members = vector(int);

	char token[256];
	int j = 1;
	push_back_int(&members, i);
	while (j < maxgroupsize - 1 && getNext(&buffer, token))
	{
		int cid;
		if (!getInt(token, &cid))
		{
			// invalid client id provided
			char out[256];
			sprintf(out, "server: invalid client id provided.");
			writeToClient(i, out);
			return;
		}

		int k = getClientIndex(cid);

		if (k == numclients)
		{
			// client does not exist
			char out[256];
			sprintf(out, "client %d does not exist.", cid);
			writeToClient(i, out);
			return;
		}

		push_back_int(&members, k);
	}

	if (j == maxgroupsize && getNext(&buffer, token))
	{
		// max group size limit is 5
		char out[256];
		sprintf(out, "server: max group size limit is %d.", maxgroupsize);
		writeToClient(i, out);
		return;
	}

	// perform action
	int g = getIndexForNewGroup(numgroups, groups);
	if (g == -1)
	{
		// no indices left for a new group
		// should never come here
		cleanup();
		exit(1);
	}

	int gid = initGroupReq(&groups[g], i, &members);

	char out[256];
	sprintf(out, "You are requested by client %d to join group %d.", clients[i].uid, gid);
	for (int j = 1; j < members.size; j++)
	{
		int client = members.a[j];
		writeToClient(client, out);
	}

	free_vector_int(&members);
}

void replyGroupReq(int reply, int i, char buffer[])
{
	char gid_s[256];
	if (!getNext(&buffer, gid_s))
	{
		// handle error
		char out[256];
		strcpy(out, "server: group id not provided");
		writeToClient(i, out);
		return;
	}

	int gid;
	if (!getInt(gid_s, &gid))
	{
		// handle error
		char out[256];
		strcpy(out, "server: invalid group id");
		writeToClient(i, out);
		return;
	}

	int g = getGroupIndex(gid);
	if (g == -1)
	{
		// handle error
		char out[256];
		strcpy(out, "server: group does not exists.");
		writeToClient(i, out);
		return;
	}

	int index;
	for (index = 0; index < groups[g].requested_members.size; index++)
	{
		if (i == groups[g].requested_members.a[index])
		{
			break;
		}
	}

	if (index == groups[g].requested_members.size)
	{
		// handle error
		char out[256];
		sprintf(out, "server: You were not requested for joining group %d.", gid);
		writeToClient(i, out);
		return;
	}

	groups[g].replies.a[index] = reply;

	bool allRepliesReceived = true;
	for (int j = 0; j < groups[g].replies.size; j++)
	{
		if (groups[g].replies.a[j] == -1)
		{
			allRepliesReceived = false;
			break;
		}
	}

	if (allRepliesReceived)
	{
		ActivateGroup(&groups[g]);

		char out[256];
		sprintf(out, "You have been added to group %d.", gid);
		for (int j = 0; j < maxgroupsize; j++)
		{
			int client = groups[g].members[j];
			if (client == -1)
			{
				continue;
			}

			// set gid in client
			int index = getIndexForNewGroupInClient(&clients[client]);
			if (index == -1)
			{
				// should never happen!
				// client already joined max number of groups
				char out[256];
				sprintf(out, "client %d already joined max number of groups.", client);
				writeToClient(i, out);
				continue;
			}
			clients[client].groups[index] = gid;

			writeToClient(client, out);
		}
	}
}

void removeFromGroup(int i, char buffer[])
{
	char gid_s[256];
	if (!getNext(&buffer, gid_s))
	{
		// handle error
		char out[256];
		strcpy(out, "server: group id not provided");
		writeToClient(i, out);
		return;
	}

	int gid;
	if (!getInt(gid_s, &gid))
	{
		// handle error
		char out[256];
		strcpy(out, "server: invalid group id");
		writeToClient(i, out);
		return;
	}

	int g = getGroupIndex(gid);
	if (g == -1)
	{
		// handle error
		char out[256];
		strcpy(out, "server: group does not exists.");
		writeToClient(i, out);
		return;
	}

	// to store indices of clients[]
	int* members = (int*)calloc(maxgroupsize, sizeof(int));
	memset(members, -1, maxgroupsize * sizeof(int));

	char token[256];
	int j = 0;
	while (j < maxgroupsize && getNext(&buffer, token))
	{
		int clientid;
		if (!getInt(token, &clientid))
		{
			// invalid client id provided
			char out[256];
			sprintf(out, "server: invalid client id provided.");
			writeToClient(i, out);
			return;
		}

		int k;
		for (k = 0; k < numclients; k++)
		{
			if (clients[k].active && clients[k].uid == clientid)
			{
				break;
			}
		}

		if (k == numclients)
		{
			// client does not exist
			char out[256];
			sprintf(out, "client %d does not exist.", clientid);
			writeToClient(i, out);
			return;
		}

		members[j++] = k;
	}

	if (j == maxgroupsize && getNext(&buffer, token))
	{
		// max group size limit is 5
		char out[256];
		sprintf(out, "server: max group size limit is %d.", maxgroupsize);
		writeToClient(i, out);
		return;
	}

	for (int j = 0; j < maxgroupsize; j++)
	{
		if (members[j] == -1)
		{
			continue;
		}

		int client = members[j];
		int res = removeMember(&groups[g], i, client);
		if (res == -3)
		{
			// no privileges
			char out[256];
			sprintf(out, "server: !!You are not an admin!!");
			writeToClient(i, out);
			return;
		}
		else if (res == -1)
		{
			// not a member
			char out[256];
			sprintf(out, "server: client %d is not a member of group %d.", clients[client].uid, gid);
			writeToClient(i, out);
			continue;
		}
		else
		{
			// send responses to the client added

			// remove gid from client
			for (int k = 0; k < numgroups; k++)
			{
				if (clients[client].groups[k] == gid)
				{
					clients[client].groups[k] = -1;
					break;
				}
			}

			char out[256];
			sprintf(out, "You have been removed from the group %d by client %d.", gid, clients[i].uid);
			writeToClient(client, out);

			bool allAdminsInactive = true;
			for (int k = 0; k < maxgroupsize; k++)
			{
				if (groups[g].admins[k] != -1)
				{
					allAdminsInactive = false;
				}
			}

			if (allAdminsInactive)
			{
				// deactivate the group
				char out[256];
				sprintf(out, "group %d has be deactivated.", gid);
				for (int k = 0; k < maxgroupsize; k++)
				{
					int client = groups[g].members[k];
					if (client == -1)
					{
						continue;
					}

					for (int l = 0; l < maxgroupsize; l++)
					{
						if (clients[client].groups[l] == gid)
						{
							clients[client].groups[l] = -1;
						}
					}

					writeToClient(client, out);
					groups[g].members[k] = -1;
				}
				groups[g].status = offline;
			}
		}
	}

	free(members);
}

void makeAdminReq(int i, char buffer[])
{
	char gid_s[256];
	if (!getNext(&buffer, gid_s))
	{
		// handle error
		char out[256];
		strcpy(out, "server: group id not provided");
		writeToClient(i, out);
		return;
	}

	int gid;
	if (!getInt(gid_s, &gid))
	{
		// handle error
		char out[256];
		strcpy(out, "server: invalid group id");
		writeToClient(i, out);
		return;
	}

	int g = getGroupIndex(gid);
	if (g == -1)
	{
		// handle error
		char out[256];
		strcpy(out, "server: group does not exists.");
		writeToClient(i, out);
		return;
	}

	char out[256];
	sprintf(out, "server: client %d has requested to be an admin for group %d.", clients[i].uid, gid);

	if (!isMember(&groups[g], i))
	{
		sprintf(out, "server: You are not a member of the group %d.", gid);
		writeToClient(i, out);
		return;
	}

	if (isAdmin(&groups[g], i))
	{
		// client is already an admin
		sprintf(out, "server: You are already an admin of the group %d.", gid);
		writeToClient(i, out);
		return;
	}

	adminreq req = {.client = i, .admins = vector(int)};
	for (int j = 0; j < maxgroupsize; j++)
	{
		int admin = groups[g].admins[j];
		if (admin == -1)
		{
			continue;
		}
		push_back_int(&req.admins, admin);

		writeToClient(admin, out);
	}
	req.replies = init_vector(int, req.admins.size, -1);
	push_back_adminreq(&groups[g].adminreqs, req);
}

void replyAdminReq(int reply, int i, char buffer[])
{
	char gid_s[256];
	if (!getNext(&buffer, gid_s))
	{
		// handle error
		char out[256];
		strcpy(out, "server: group id not provided");
		writeToClient(i, out);
		return;
	}

	int gid;
	if (!getInt(gid_s, &gid))
	{
		// handle error
		char out[256];
		strcpy(out, "server: invalid group id");
		writeToClient(i, out);
		return;
	}

	int g = getGroupIndex(gid);
	if (g == -1)
	{
		// handle error
		char out[256];
		strcpy(out, "server: group does not exists.");
		writeToClient(i, out);
		return;
	}

	char cid_s[256];
	if (!getNext(&buffer, cid_s))
	{
		// handle error
		char out[256];
		strcpy(out, "server: client id not provided");
		writeToClient(i, out);
		return;
	}

	int cid;
	if (!getInt(cid_s, &cid))
	{
		// handle error
		char out[256];
		strcpy(out, "server: invalid client id");
		writeToClient(i, out);
		return;
	}

	int c = getClientIndex(cid);
	if (c == -1)
	{
		// handle error
		char out[256];
		strcpy(out, "server: client does not exist.");
		writeToClient(i, out);
		return;
	}

	int index = -1;
	for (int j = 0; j < groups[g].adminreqs.size; j++)
	{
		if (groups[g].adminreqs.a[j].client == c)
		{
			index = j;
			break;
		}
	}

	if (index == -1)
	{
		char out[256];
		sprintf(out, "server: group %d has no admin request from client %d.", gid, cid);
		writeToClient(i, out);
		return;
	}

	int admin = -1;
	for (int j = 0; j < groups[g].adminreqs.a[index].admins.size; j++)
	{
		if (groups[g].adminreqs.a[index].admins.a[j] == i)
		{
			admin = j;
			break;
		}
	}

	if (admin == -1)
	{
		char out[256];
		sprintf(out, "server: You were not requested for admin approval of client %d for group %d.", cid, gid);
		writeToClient(i, out);
		return;
	}

	groups[g].adminreqs.a[index].replies.a[admin] = reply;

	bool allAdminsReplied = true;
	for (int j = 0; j < groups[g].adminreqs.a[index].replies.size; j++)
	{
		if (groups[g].adminreqs.a[index].replies.a[j] == -1)
		{
			allAdminsReplied = false;
			break;
		}
	}

	if (allAdminsReplied)
	{
		int approvals = 0;
		for (int j = 0; j < groups[g].adminreqs.a[index].replies.size; j++)
		{
			if (groups[g].adminreqs.a[index].replies.a[j] == 1)
			{
				approvals++;
			}
		}

		char out[256];
		if (approvals >= (groups[g].adminreqs.a[index].replies.size + 1) / 2)
		{
			int res = addAdmin(&groups[g], i, c);
			if (res == -3)
			{
				// no privileges
				strcpy(out, "server: !!You are not an admin!!");
			}
			else if (res == -1)
			{
				// client is not a member
				sprintf(out, "server: client %d is not a member of group %d.", cid, gid);
			}
			else if (res == -2)
			{
				// client is already an admin
				sprintf(out, "server: client %d is already an admin of group %d.", cid, gid);
			}
			else
			{
				sprintf(out, "server: You are now an admin of group %d.", gid);
			}
		}
		else
		{
			sprintf(out, "server: Your admin request for the group %d was rejected.", gid);
		}
		writeToClient(c, out);

		// free
		free_vector_int(&groups[g].adminreqs.a[index].admins);
		free_vector_int(&groups[g].adminreqs.a[index].replies);
		groups[g].adminreqs.a[index].client = -1;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////
// Functions to process client message

int performAction(int i, char* buffer)
{
	char command[256];
	if (!getNext(&buffer, command))
	{
		char out[256];
		strcpy(out, "server: invalid command");
		writeToClient(i, out);
		return fdmax;
	}

	if (strcmp(command, "/quit\n") == 0)
	{
		removeClient(i);
		
		printf("client %d left the chat.\n", i + 1);
	}

	else if (strcmp(command, "/active\n") == 0)
	{
		char out[265];
		strcpy(out, "server: ");
		// get active clients
		for (int j = 0; j < numclients; j++)
		{
			if (i != j && clients[j].active)
			{
				char uid[6];
				sprintf(uid, "%d", clients[j].uid);
				strcat(out, uid);
				strcat(out, " ");
			}
		}

		if (strcmp(out, "server: ") == 0)
		{
			strcat(out, "No other clients are active.");
		}

		// send to client
		writeToClient(i, out);
	}

	else if (strcmp(command, "/broadcast") == 0)
	{
		broadcast(i, buffer);
	}

	else if (strcmp(command, "/send") == 0)
	{
		sendMsg(i, buffer);
	}

	else if (strcmp(command, "/makegroup") == 0)
	{
		makeGroup(i, buffer);
	}

	else if (strcmp(command, "/sendgroup") == 0)
	{
		sendGroup(i, buffer);
	}

	else if (strcmp(command, "/activegroups\n") == 0)
	{
		activeGroup(i, buffer);
	}

	else if (strcmp(command, "/makeadmin") == 0)
	{
		makeAdmin(i, buffer);
	}

	else if (strcmp(command, "/addtogroup") == 0)
	{
		addToGroup(i, buffer);
	}
	
	else if (strcmp(command, "/makegroupbroadcast") == 0)
	{
		makeGroupBroadcast(i, buffer);
	}

	else if (strcmp(command, "/makegroupreq") == 0)
	{
		makeGroupReq(i, buffer);
	}

	else if (strcmp(command, "/joingroup") == 0)
	{
		replyGroupReq(1, i, buffer);
	}

	else if (strcmp(command, "/declinegroup") == 0)
	{
		replyGroupReq(0, i, buffer);
	}

	else if (strcmp(command, "/removefromgroup") == 0)
	{
		removeFromGroup(i, buffer);
	}

	else if (strcmp(command, "/makeadminreq") == 0)
	{
		makeAdminReq(i, buffer);
	}

	else if (strcmp(command, "/approveadminreq") == 0)
	{
		replyAdminReq(1, i, buffer);
	}

	else if (strcmp(command, "/declineadminreq") == 0)
	{
		replyAdminReq(0, i, buffer);
	}

	else
	{
		char out[256];
		sprintf(out, "server: invalid command");
		writeToClient(i, out);
	}

	return fdmax;
}


int processClient(int i)
{
	char buffer[256];
	// receive message from client
	recv(clients[i].sockfd, buffer, 256, 0);
	// printf("recvd from client %d: %s\n", i + 1, buffer);

	return performAction(i, buffer);
}

///////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////
// Init functions

int registerClient(int newsockfd)
{
	// get position for new client
	int i = getIndexForNewClient();
	clients[i].active = true;
	clients[i].sockfd = newsockfd;
	clients[i].uid = rand() % 89999 + 10000;
	
	// find max fd value
	fdmax = getMaxFd();
	
	FD_SET(clients[i].sockfd, &master);

	char out[256];
	sprintf(out, "server: %d", clients[i].uid);
	write(clients[i].sockfd, out, sizeof(out));

	printf("client %d enterd the chat.\n", i + 1);

	return fdmax;
}

void initClients()
{
	clients = (client*)calloc(numclients, sizeof(client));
	for (int i = 0; i < numclients; i++)
	{
		clients[i].groups = (int*)calloc(numgroups, sizeof(int));
		memset(clients[i].groups, -1, numgroups * sizeof(int));
	}
}

void initGroups()
{
	groups = (group*)calloc(numgroups, sizeof(group));
	for (int i = 0; i < numgroups; i++)
	{
		groups[i].admins  = (int*)calloc(maxgroupsize, sizeof(int));
		groups[i].members = (int*)calloc(maxgroupsize, sizeof(int));
		memset(groups[i].admins, -1, maxgroupsize * sizeof(int));
		memset(groups[i].members, -1, maxgroupsize * sizeof(int));
	}
}

void initServerSocket(char port[10])
{
	int portno, pid, flags;
	struct sockaddr_in serv_addr;
	  
	sockfd = socket(AF_INET, SOCK_STREAM, 0);   // getting a sockfd for a TCP connection
	if (sockfd < 0)  perror("ERROR opening socket");

	int optval = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	if ((flags = fcntl(sockfd, F_GETFL, 0)) < 0) 
	{ 
		perror("can't get flags to SCOKET!!");
	} 


	if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) 
	{ 
		perror("fcntl failed.");
	}

	bzero((char *) &serv_addr, sizeof(serv_addr));

	portno = atoi(port);
	serv_addr.sin_family = AF_INET;  // symbolic constant for IPv4 address
	serv_addr.sin_addr.s_addr = INADDR_ANY;  // symbolic constant for holding IP address of the server
	serv_addr.sin_port = htons(portno);

	if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
	{
		perror("ERROR on binding");
 	}
}

///////////////////////////////////////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{
	srand(time(0));
	
	signal(SIGINT, sigCHandler);
	signal(SIGTSTP, sigZhandler);

	char port[10] = "5000";
	if (argc == 2)
	{
		strcpy(port, argv[1]);
	}
	initServerSocket(port);

	initClients();
	initGroups();
 	
	// setup server to listen
 	listen(sockfd, numclients);

	// init clint socket file descriptors
	fd_set readfds;
 	FD_ZERO(&readfds);

	// intercept on a new connection
	FD_SET(sockfd, &master);

	// client sockfds
	fdmax = sockfd;

	// the n param in select()
	int n = fdmax + 1;

	// main loop
	while (1) 
	{
		readfds = master;  // copy
		int activity = select(n, &readfds, NULL, NULL, NULL);  //give appropriate parameters
		
		if((activity< 0)&&(errno != 0))  //fill appropriate parameters here
		{
			perror("select");
		}
		else
		{
			/*
			  After successful select call we can now monitor these two scenarios in a non-blocking fashion:
				- A new connection is established, and
				- An existing client has made some request
			*/

			if (FD_ISSET(sockfd, &readfds))
			{
				int clilen;
				struct sockaddr_in cli_addr;
				int newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
				if (newsockfd >= 0) 
				{
					if (numconnections == numclients)
					{
						char buffer[256];
						strcpy(buffer, "Connection Limit Exceeded !!");
						send(newsockfd, &buffer, sizeof(buffer), 0);
						continue;
					}

					numconnections++;
					fdmax = registerClient(newsockfd);
					n = fdmax + 1;  // to accommodate new clinet
				}
				else
				{
					perror("accept");
				}
			}

			for (int i = 0; i < numclients; i++)
			{
				if (clients[i].active && FD_ISSET(clients[i].sockfd, &readfds))
				{
					fdmax = processClient(i);
					n = fdmax + 1;  // in case a clients disconnect
				}
			}
		}

	}
	
	return 0;
}
