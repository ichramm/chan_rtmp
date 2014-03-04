#
# Makefile for chan_rtmp
#

MODULE_NAME ?=chan_rtmp

# Debug, Release
Target      ?=Release

ASTERISK_PREFIX ?=/usr
LIBRTMP_PREFIX  ?=/usr

LIBRTMP_LIBDIR    ?=$(LIBRTMP_PREFIX)/lib
LIBRTMP_LIB       ?=rtmp

CC          ?=gcc
LINKER      ?=gcc
GCC_VERSION ?= $(shell expr substr "`$(CC) -dumpversion | tr -dc '[0-9]'`" 1 2)

ASTERISK_INCLUDES ?=$(ASTERISK_PREFIX)/include
LIBRTMP_INCLUDES  ?=$(LIBRTMP_PREFIX)/include

INCLUDES :=-I$(ASTERISK_INCLUDES)
ifneq '$(LIBRTMP_INCLUDES)' '$(ASTERISK_INCLUDES)'
	INCLUDES :=$(INCLUDES) -I$(LIBRTMP_INCLUDES)
endif

CFLAGS  :=$(CFLAGS) -fPIC -g -c -Wall -Wextra -DGCC_VERSION=$(GCC_VERSION) -DAST_MODULE='"$(MODULE_NAME)"' -D_GNU_SOURCE
LDFLAGS :=$(LDFLAGS) -Wall -fPIC -g -shared

ifeq ($(origin OBJS_ROOT), undefined)
	OBJS_ROOT :=./objs
else
	OBJS_ROOT := $(OBJS_ROOT)/$(MODULE_NAME)
endif

OBJS_DIR    =$(OBJS_ROOT)/$(Target)
SRC_DIR     =./src
OUTPUT_DIR ?=./bin

ifeq (Debug, $(findstring Debug,$(Target)))
	OUTPUT_LIB ?=$(MODULE_NAME)_d.so
	CFLAGS :=$(CFLAGS) -D_DEBUG
else
	OUTPUT_LIB ?=$(MODULE_NAME).so
	CFLAGS :=$(CFLAGS) -O2
endif

OUTPUT_FILE = $(OUTPUT_DIR)/$(OUTPUT_LIB)

OBJS =$(OBJS_DIR)/chan_rtmp.o

LIBS      =-L$(LIBRTMP_LIBDIR) -l$(LIBRTMP_LIB) -lrt
COMPILE   =$(CC) $(CFLAGS) "$<" -o "$(OBJS_DIR)/$(*F).o" $(INCLUDES)
LINK      =$(LINKER) $(LDFLAGS) -o "$(OUTPUT_FILE)" $(OBJS) $(LIBS)

$(OUTPUT_FILE): $(OBJS_DIR) $(OUTPUT_DIR) $(OBJS)
	$(LINK)

$(OBJS_DIR):
	mkdir -p $(OBJS_DIR)

$(OUTPUT_DIR):
	mkdir -p $(OUTPUT_DIR)

$(OBJS_DIR)/%.o: $(SRC_DIR)/%.c
	$(COMPILE)

clean:
	rm -f $(OBJS_DIR)/*.o
	rm -f "$(OUTPUT_FILE)"

