# Multiclient Chat App
This repository contains C code for a multiclient chat console app. It contains two components:
1. Server
2. Client

## Server
A centralized server handles the business logic maintaining the clients' states. Clients can register to the server and perform several actions that allow them to chat with other clients. The following are the actions supported by the server:\
Note: All commands are in the format: /command arg1 arg2 ...
1. /active
    - Fetches all the active clients' Ids
2. /broadcast message
    - Broadcasts the given message to all clients
3. /send clientId message
    - Sends the given message to the given client
4. /makegroup clientId1 clientId2 ...
    - Creates a group with the given clients incl
    - The creater will be the admin for the group
    - If all admins leave the group, it will be deactivated
5. /sendgroup groupId message
    - Sends the given message to the given group
6. /activegroups
    - Fetches all the active groups' Ids
7. /makeadmin groupId clientId
    - Makes the given client an admin for the given group
    - Only allowed by admins of the group
8. /addtogroup groupId clientId1 clientId2 ...
    - Adds the given clients to the given group
9. /makegroupbroadcast groupId
    - Makes the given group a broadcast group where only admins can message
10. /makegroupreq clientId1 clientId2 ...
    - Requests the given clients to join a new group
11. /joingroup groupId
    - Accepts the join request for the given group
12. /declinegroup groupId
    - Declines the join request for the given group
13. /removefromgroup groupId clientId1 clientId2 ...
    - Removes the given clients from the given group
    - Only allowed by admins of the group
14. /makeadminreq groupId
    - Requests admins of the group to become an admin
    - Ony allowed by members of the group
15. /approveadminreq groupId clientId
    - Approves the given client to become an admin of the given group
16. /declineadminreq groupId clientId
    - Declines the given client to become an admin of the given group
17. /quit
    - Deregisters the client
    - The client is removed from all the present groups
    - All the previously present groups are notified of the client exit

## Client
Clients can chat with other clients registered to the server. Client console application simply accepts user input, sends it to the server and displays the messages received from the server. Client is automatically registered to the server upon application startup.

## Compilation:
This code is meant to be compiled and run in Linux/Ubuntu. Run the following commands to compile the C files:

```
gcc server.c -o s
gcc client.c -o c
```

## How to run
### Server

```./s```

where the server is run on port number 5000

(or)

```./s PORT```

where the server is run on port number PORT

### Client

```./c```

where the client attempts server connection to port 5000

(or)

```./c PORT```

where the client attempts server connection to port PORT
