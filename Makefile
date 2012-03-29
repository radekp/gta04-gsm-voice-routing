all: gsm-voice-routing

aec.o: aec.cpp
	g++ -lm -c aec_test.cpp

gsm-voice-routing: aec.o gsm-voice-routing.c
	g++ -Wall -lrt -lasound -lm -ldl -lpthread -lspeexdsp -o gsm-voice-routing gsm-voice-routing.c aec.o

clean:
	rm -f gsm-voice-routing
