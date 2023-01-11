TARGET=priptool

CFLAGS=-Wall

OBJ=priptool.o \
	   utils.o \
	   set.o \
	   unset.o \
	   show.o 

$(TARGET): $(OBJ)
	cc $^ -o $@

$(OBJ): %.o : %.c
	cc -c $< -o $@ $(CFLAGS)

clean:
	rm *.o $(TARGET)
