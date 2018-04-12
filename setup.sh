sudo tc qdisc del dev eno1 root
sudo tc qdisc del dev enp2s0 root
sudo rmmod sch_red 
make
sudo insmod sch_red.ko
sudo tc qdisc add dev enp2s0 root red bandwidth 1024KBPS limit 100000 avpkt 1000
sudo tc qdisc add dev eno1 root red bandwidth 1024KBPS limit 100000 avpkt 1000
lsmod
