FLAGS= -O0 -g -DBUILD_HOST -Wall

build: compile
	gcc $(FLAGS) -o virt_hw gsm.o sms.o sim_card.o remote_call.o \
	sysdeps_posix.o modem_driver.o android_modem.o virt_hw.o \
	console.o stralloc.o gps_client.o -lpthread

compile: stralloc.o \
	gsm.o \
	sms.o \
	sim_card.o \
	remote_call.o \
	sysdeps_posix.o \
	modem_driver.o \
	android_modem.o \
	virt_hw.o \
	console.o \
	gps_client.o 
	echo "COMPILE DONE"

virt_hw.o:
	gcc $(FLAGS) -c virt_hw.c
gsm.o:
	gcc $(FLAGS) -c gsm.c
sms.o:
	gcc $(FLAGS) -c sms.c
sim_card.o:
	gcc $(FLAGS) -c sim_card.c
remote_call.o:
	gcc $(FLAGS) -c remote_call.c
sysdeps_posix.o:
	gcc $(FLAGS) -c sysdeps_posix.c
modem_driver.o:
	gcc $(FLAGS) -c modem_driver.c
android_modem.o:
	gcc $(FLAGS) -c android_modem.c
console.o:
	gcc $(FLAGS) -c console.c
stralloc.o:
	gcc $(FLAGS) -c stralloc.c
gps_client.o:
	gcc $(FLAGS) -c gps_client.c
utils.o:
	gcc $(FLAGS) -c utils.c
	
clean:
	- rm *.o
	- rm virt_hw