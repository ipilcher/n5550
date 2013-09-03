KERNEL = $(shell uname -r)

obj-m += n5550_board.o n5550_ahci_leds.o

all:
	make -C /lib/modules/$(KERNEL)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KERNEL)/build M=$(PWD) clean
