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

#
# nlohmann/json is a TRANSITIVE build dependency, not ours: the Enabler's own
# public header ddsenabler_participants/Writer.hpp includes <nlohmann/json.hpp>,
# so anything compiling against the Enabler needs it present. Nothing here uses
# it directly, which is exactly why its absence is confusing - the error names a
# file in /usr/local/include that we did not write.
#
# Debian/Ubuntu: nlohmann-json3-dev
#
DDS_JSON_HEADER = /usr/include/nlohmann/json.hpp

#
# And the same story again: ddspipe_yaml/Yaml.hpp includes <yaml-cpp/yaml.h>.
# Found by a docker build that had nlohmann and not this one, and failed with
# a fatal error naming ddspipe_yaml.
#
# Debian/Ubuntu: libyaml-cpp-dev
#
DDS_YAML_HEADER = /usr/include/yaml-cpp/yaml.h

ifeq ($(COR_BRIDGE_DDS),auto)
  ifeq ($(and $(wildcard $(DDS_HEADER)),$(wildcard $(DDS_LIB)),$(wildcard $(DDS_JSON_HEADER)),$(wildcard $(DDS_YAML_HEADER))),)
    COR_BRIDGE_DDS := OFF
    DDS_SKIP_REASON := the DDS Enabler is not installed
  else
    COR_BRIDGE_DDS := ON
  endif
endif

PLUGIN        = dds.so
CXX           = g++
#
# The directory the sibling repos live in, which is simply the parent: this repo
# sits beside corBridge on a workstation (~/git/...) and beside it on a CI
# runner (<workspace>/stack/...) alike. It was $(HOME)/git, which is true of
# exactly one of those and failed on the other with "corBridge/BridgeDriver.h:
# No such file or directory".
#
COR_LIBS     ?= ..
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
	@test -f $(DDS_JSON_HEADER) || { echo "corDdsBridge: $(DDS_JSON_HEADER) not found - the Enabler's own headers include <nlohmann/json.hpp> (apt: nlohmann-json3-dev)"; exit 1; }
	@test -f $(DDS_YAML_HEADER) || { echo "corDdsBridge: $(DDS_YAML_HEADER) not found - the Enabler's own headers include <yaml-cpp/yaml.h> (apt: libyaml-cpp-dev)"; exit 1; }

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

#
# contract - does this plugin still satisfy BridgeDriver.h?
#
# ⭐ NEEDS NO DDS AT ALL, and that is the point. ddsRegister.cpp includes only
# ddsBridge.hpp, which includes only corBridge's two headers - so the file that
# fills in the BridgeDriver struct, and therefore the file that breaks the
# moment the contract changes, compiles anywhere corBridge is checked out.
#
# That makes it cheap enough to run on every pull request, which is what stops
# this repo rotting. Without it a change to BridgeDriver.h would build clean in
# coraine, skip silently here (no Enabler on a CI runner, so auto turns the
# build OFF) and be discovered by hand, later, by whoever next needed DDS.
#
# The full build still requires the Enabler. This checks the seam, not the
# transport.
#
contract:
	@$(CXX) -std=c++17 -Wall -Werror -fPIC -I$(COR_LIBS) -c ddsRegister.cpp -o /tmp/corDdsBridge-contract.o
	@rm -f /tmp/corDdsBridge-contract.o
	@echo "corDdsBridge: contract OK - BridgeDriver.h is still satisfied"

.PHONY: all install di ci clean ddsCheck contract

-include $(DEPS)
