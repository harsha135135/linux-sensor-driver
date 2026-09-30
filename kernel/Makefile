# Out-of-tree kbuild wrapper. KDIR can point at any configured kernel tree.
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# Static analysis with sparse (C=2 checks every file regardless of rebuild).
sparse:
	$(MAKE) -C $(KDIR) M=$(CURDIR) C=2 CF="-D__CHECK_ENDIAN__" modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all sparse clean
