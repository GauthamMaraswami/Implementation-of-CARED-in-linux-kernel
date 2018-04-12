# Implementation of CARED Algorithm in linux kernel
## course code :CO365
## Course title :Advanced Computer Networking
### Course Instructor : Dr.Mohit P tahiliani
## steps to compile
step 1 : pull the files sch_red.c and make to a specified location
step 2 : enable packet transfer from client to server system
step 3 : run make
step 4 : insmod sch_red.ko
step 5 : tc qdisc add dev "interfaceclient" root red limits 100000 avpkt 1000 bandwidth 1024KBPS
step 6 : tc qdisc add dev "interface for stats" root red limits 100000 avpkt 1000 bandwidth 1024KBPS
step 7 : start netserver in server
step 8 : send packets from client using flent command
step 9 :./run-flent rrul -p total -l 160 -H [SERVER_IP] --test-parameter bandwidth=800M --test-parameter
qdisc_stats_hosts=[ROUTER_SSH_IP using which ssh connection is setup]
--test-parameter upload_streams=num_cpus --test-parameter download_streams=num_cpus -t 
RARED -o cared.png
step 10 : watch the interface in server for packet loss


## references
Implimentation of RARED in Linux Kernel
https://github.com/ShikhaBakshi/Implementation-of-RARED-in-Linux-Kernel

Linux Kernel Queue Discipilne Algorithms Locations
https://elixir.bootlin.com/linux/latest/source/net/sched

Journel Paper on CARED 
https://www.sciencedirect.com/science/article/pii/S1084804511002359

ns-3 implimentation of CARED
https://github.com/bhattmansi/Implementation-of-CARED-in-ns3

