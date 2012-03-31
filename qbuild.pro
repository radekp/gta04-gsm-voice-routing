TEMPLATE=app
TARGET=gsm-voice-routing

CONFIG+=qtopia
LIBS+=-lrt -lasound -lm -ldl -lpthread -lspeexdsp
DEFINES+=QTOPIA

# I18n info
STRING_LANGUAGE=en_US
LANGUAGES=en_US

# Package info
pkg [
    name=gsm-voice-routing
    desc="Voice routing for gsm call on GTA04"
    license=LGPL
    version=1.0
    maintainer="Radek Polak <psonek2@seznam.cz>"
]

SOURCES=gsm-voice-routing.c

# Install rules
target [
    hint=sxe
    domain=untrusted
]