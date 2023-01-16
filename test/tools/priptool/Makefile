TARGET=priptool

CFLAGS  = -Wall -std=gnu11
LDFLAGS = -lpthread -lpcap

OBJS=priptool.o \
	   set.o \
	   unset.o \
	   utils.o \
	   show.o \
	   jhash.o \
	   timer.o \
	   latency.o

$(TARGET): $(OBJS)
	cc $^ -o $@ $(LDFLAGS)

linxnethatool.o : set.h unset.h show.h latency.h list.h

set.o : set.h utils.h

unset.o : unset.h utils.h

show.o : show.h utils.h list.h

jhash.o : jhash.h list.h

timer.o : timer.h list.h

latency.o : latency.h utils.h jhash.h timer.h list.h

.PHONY:
clean:
	rm $(OBJS) $(TARGET)
