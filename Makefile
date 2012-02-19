all: gsm-voice-routing

gsm-voice-routing: gsm-voice-routing.c
	gcc -Wall -lrt -lasound -lm -ldl -lpthread -lspeexdsp -o gsm-voice-routing gsm-voice-routing.c

clean:
	rm -f gsm-voice-routing
