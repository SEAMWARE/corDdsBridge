#
# FILE            makefile
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# corDdsBridge is a PLUGIN, and it is built on its own - never from the corLibs
# umbrella. The eProsima stack it links is 21.4 MiB across nine libraries,
# roughly five times the whole broker, and putting it in COR_DIRS would make all
# of that a hard dependency of every coraine clone. That is the precise thing
# the plugin decision exists to prevent.
#
# It is also the only C++ in the stack, for a reason that is not stylistic: the
# DDS Enabler's API takes std::string and std::shared_ptr, which no amount of
# declaring mangled symbols reaches from C.
#
#
# Whether to build at all is a CONFIGURATION decision, not a question of which
# list this repo appears in.
#
#   auto  (default) build if the DDS Enabler is installed, and say so either way
#   ON             build, and fail loudly if it is not there
#   OFF            do not build
#
# auto exists because the umbrella walks every sibling repo and a developer
# without the eProsima stack must still get a working build - but it PRINTS what
# it decided. A build that quietly produces nothing is how a plugin stops
# existing without anybody noticing.
#
COR_BRIDGE_DDS ?= auto

DDS_HEADER     = /usr/local/include/ddsenabler/DDSEnabler.hpp
DDS_LIB        = /usr/local/lib/libddsenabler.so

ifeq ($(COR_BRIDGE_DDS),auto)
  ifeq ($(and $(wildcard $(DDS_HEADER)),$(wildcard $(DDS_LIB))),)
    COR_BRIDGE_DDS := OFF
    DDS_SKIP_REASON := the DDS Enabler is not installed
  else
    COR_BRIDGE_DDS := ON
  endif
endif

PLUGIN        = dds.so
CXX           = g++
COR_LIBS     ?= $(HOME)/git
PLUGIN_DIR   ?= /opt/seamware/plugins/bridge

INCLUDE       = -I$(COR_LIBS) -I/usr/local/include
CXXFLAGS      = -std=c++17 -O2 -Wall -Werror -fPIC $(INCLUDE) -MMD -MP

#
# The broker is linked rdynamic, so ktrace and the rest resolve from the running
# process at dlopen. Only the transport's own libraries are linked here.
#
LDFLAGS       = -L/usr/local/lib
LIBS          = -lddsenabler -lddsenabler_participants -lddsenabler_yaml \
                -lddspipe_core -lddspipe_participants -lddspipe_yaml \
                -lcpp_utils -lfastdds -lfastcdr -lpthread

SOURCES       = ddsBridge.cpp ddsRegister.cpp
OBJS          = $(SOURCES:.cpp=.o)
DEPS          = $(SOURCES:.cpp=.d)

ifeq ($(COR_BRIDGE_DDS),OFF)

all install di ci clean:
	@echo "corDdsBridge: not built$(if $(DDS_SKIP_REASON), - $(DDS_SKIP_REASON))"
	@echo "corDdsBridge: build it with  make COR_BRIDGE_DDS=ON"

else

all: ddsCheck $(PLUGIN)

#
# ON means somebody asked for it, so a missing dependency is an error and not a
# reason to carry on quietly.
#
ddsCheck:
	@test -f $(DDS_HEADER) || { echo "corDdsBridge: $(DDS_HEADER) not found - the DDS Enabler must be installed"; exit 1; }
	@test -f $(DDS_LIB)    || { echo "corDdsBridge: $(DDS_LIB) not found - the DDS Enabler must be installed"; exit 1; }

$(PLUGIN): $(OBJS)
	$(CXX) -shared $(OBJS) -o $(PLUGIN) $(LDFLAGS) $(LIBS) -Wl,-rpath,/usr/local/lib

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

install: all
	mkdir -p $(PLUGIN_DIR)
	cp -p $(PLUGIN) $(PLUGIN_DIR)/

di: install
ci: clean install

clean:
	rm -f *.o *.d *.so *~

endif

.PHONY: all install di ci clean ddsCheck

-include $(DEPS)
