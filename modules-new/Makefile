KERNEL = $(shell uname -r)

obj-m += n5550_board.o ledtrig-blkdev.o

all:
	make -C /lib/modules/$(KERNEL)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KERNEL)/build M=$(PWD) clean
