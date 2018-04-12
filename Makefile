obj-m += sch_red.o

all:
	make -C /lib/modules/4.15.0/build M=$(PWD) modules

clean:
	make -C /lib/modules/4.15.0/build M=$(PWD) clean
