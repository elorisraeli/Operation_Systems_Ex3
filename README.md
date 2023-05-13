# Operation_Systems_Ex3

How to use the project:

First step is to run all the project using Makefile.
To use the Makefile you should enter the command: make

if you want to run it by yourself, enter the command: gcc -o stnc stnc.c

and it will generate a stnc file which you could run using ./stnc and with the instructions below.

Usage by parts (what commands to run in terminal):


## Part A - chat cmd tool

Can send messages over the network, to the same tool ,listening on the
other side, and get the response, so there will be 2 sides communication. The usage s like this:
- The server side: stnc -s PORT
- The client side: stnc -c IP PORT

the communication done using IPv4 TCP protocol. 
“chat” means a tool that can read input from keyboard, and in the same time listen to the socket
of the other side.

> Example command:
>
> Server: ./stnc -s 8080
>
> Client: ./stnc -c 127.0.0.1 8080
>
> and now each side can send messages throw the keyboard and press enter to sent to other side.

To quit (exit) from the stnc part A program, enter the command: exit

## Part B - file transfer

To enter this part in stnc program, you will use the option -p (performance)
In the program there are several commands (parameters and types):

The server side (the receiver): 
- ./stnc -s PORT -p 
- ./stnc -s PORT -p -q

when the -q is for quiet mode which prints only the summery time took the file to transfer (in server side).

The client side (the sender):
- ./stnc -c IP PORT -p <param_value> <type_value>
- ./stnc -c IP PORT -p <type_value> <param_value> 
- ./stnc -c IP PORT <param_value> -p <type_value>
- ./stnc -c IP PORT <type_value> -p <param_value>
- ./stnc -c IP PORT <param_value> <type_value> -p
- ./stnc -c IP PORT <type_value> <param_value> -p

All the options in the param and type are these:

<type> will be the communication types: can be ipv4,ipv6,mmap,pipe,uds.
  
<param> will be a parameter for the type. It can be udp/tcp or dgram/stream or file name:

You have 8 combinations:
ipv4 tcp
ipv4 udp
ipv6 tcp
ipv6 udp
uds dgram
uds stream
mmap filename
pipe filename

> Example of using part B:
>
> Server: ./stnc -s 9090 -p
>
> Client: ./stnc -c 127.0.0.1 -s -p udp ipv6

