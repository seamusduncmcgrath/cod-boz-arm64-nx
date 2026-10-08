# Host-side unit tests of the parts of the loader that do not need a console. The Switch build
# itself is CMake's: see CMakeLists.txt and build.bat.
HOST_CC ?= cc
CLANG_FORMAT ?= clang-format
HOST_TEST_CFLAGS ?= -O0 -g
HOST_TEST_LDFLAGS ?=
BUILD_DIR ?= build/host
HOST_TESTS := \
  $(BUILD_DIR)/tests/s3e_socket_test \
  $(BUILD_DIR)/tests/s3e_config_test \
  $(BUILD_DIR)/tests/mdns_wire_test \
  $(BUILD_DIR)/tests/s3e_zeroconf_test \
  $(BUILD_DIR)/tests/s3e_timer_test \
  $(BUILD_DIR)/tests/s3e_memory_test \
  $(BUILD_DIR)/tests/device_id_test \
  $(BUILD_DIR)/tests/s3e_audio_test \
  $(BUILD_DIR)/tests/s3e_audio_unit_test \
  $(BUILD_DIR)/tests/s3e_input_test
PROJECT_CPPFLAGS := -D_GNU_SOURCE -Iinclude
PROJECT_CFLAGS := -std=c11 -Wall -Wextra -Werror
FORMAT_FILES := $(shell find include src tests -type f \( -name '*.c' -o -name '*.h' \) | sort)

test: $(HOST_TESTS)
	@set -e; for test in $(HOST_TESTS); do "$$test"; done

test-sanitize:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/sanitize \
	  HOST_TEST_CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
	  HOST_TEST_LDFLAGS="-fsanitize=address,undefined" test

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/tests/s3e_socket_test: tests/s3e_socket_test.c src/s3e_socket.c src/s3e_config.c \
                     include/s3e_host_internal.h | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(HOST_CC) $(PROJECT_CPPFLAGS) $(HOST_TEST_CFLAGS) \
	  $(PROJECT_CFLAGS) -pthread \
	  -o $@ tests/s3e_socket_test.c src/s3e_socket.c src/s3e_config.c \
	  $(HOST_TEST_LDFLAGS)

$(BUILD_DIR)/tests/s3e_config_test: tests/s3e_config_test.c src/s3e_config.c \
                                     include/s3e_host_internal.h | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(HOST_CC) $(PROJECT_CPPFLAGS) $(HOST_TEST_CFLAGS) $(PROJECT_CFLAGS) \
	  -o $@ tests/s3e_config_test.c src/s3e_config.c $(HOST_TEST_LDFLAGS)

$(BUILD_DIR)/tests/mdns_wire_test: tests/mdns_wire_test.c src/mdns_wire.c \
                                   include/mdns_wire.h | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(HOST_CC) $(PROJECT_CPPFLAGS) $(HOST_TEST_CFLAGS) $(PROJECT_CFLAGS) -o $@ \
	  tests/mdns_wire_test.c src/mdns_wire.c $(HOST_TEST_LDFLAGS)

$(BUILD_DIR)/tests/s3e_zeroconf_test: tests/s3e_zeroconf_test.c \
                                       tests/zeroconf_platform_fake.c \
                                       src/s3e_zeroconf.c src/mdns_wire.c \
                                       include/s3e_host_internal.h include/mdns_wire.h \
                                       include/zeroconf_platform.h | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(HOST_CC) $(PROJECT_CPPFLAGS) -Itests $(HOST_TEST_CFLAGS) \
	  $(PROJECT_CFLAGS) -o $@ tests/s3e_zeroconf_test.c \
	  tests/zeroconf_platform_fake.c src/s3e_zeroconf.c src/mdns_wire.c \
	  $(HOST_TEST_LDFLAGS)

$(BUILD_DIR)/tests/s3e_timer_test: tests/s3e_timer_test.c src/s3e_timer.c \
                                    include/s3e_host_internal.h | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(HOST_CC) $(PROJECT_CPPFLAGS) $(HOST_TEST_CFLAGS) $(PROJECT_CFLAGS) -pthread \
	  -o $@ tests/s3e_timer_test.c src/s3e_timer.c $(HOST_TEST_LDFLAGS)

$(BUILD_DIR)/tests/s3e_memory_test: tests/s3e_memory_test.c src/s3e_memory.c \
                                     include/s3e_host_internal.h | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(HOST_CC) $(PROJECT_CPPFLAGS) $(HOST_TEST_CFLAGS) $(PROJECT_CFLAGS) -pthread \
	  -o $@ tests/s3e_memory_test.c src/s3e_memory.c $(HOST_TEST_LDFLAGS)

$(BUILD_DIR)/tests/device_id_test: tests/device_id_test.c src/device_id.c \
                                  include/s3e_host_internal.h | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(HOST_CC) $(PROJECT_CPPFLAGS) $(HOST_TEST_CFLAGS) $(PROJECT_CFLAGS) -pthread \
	  -o $@ tests/device_id_test.c src/device_id.c $(HOST_TEST_LDFLAGS)

$(BUILD_DIR)/tests/s3e_audio_test: tests/s3e_audio_test.c src/s3e_audio.c \
                                    include/s3e_host_internal.h | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(HOST_CC) $(PROJECT_CPPFLAGS) -Ddlsym=s3e_audio_test_dlsym \
	  -Ddlclose=s3e_audio_test_dlclose $(HOST_TEST_CFLAGS) $(PROJECT_CFLAGS) \
	  -o $@ tests/s3e_audio_test.c src/s3e_audio.c $(HOST_TEST_LDFLAGS)

$(BUILD_DIR)/tests/s3e_audio_unit_test: tests/s3e_audio_unit_test.c src/s3e_audio_unit.c \
                                         include/s3e_host_internal.h | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(HOST_CC) $(PROJECT_CPPFLAGS) $(HOST_TEST_CFLAGS) $(PROJECT_CFLAGS) \
	  -o $@ tests/s3e_audio_unit_test.c src/s3e_audio_unit.c $(HOST_TEST_LDFLAGS)

$(BUILD_DIR)/tests/s3e_input_test: tests/s3e_input_test.c src/s3e_input.c \
                                      include/s3e_host_internal.h | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(HOST_CC) $(PROJECT_CPPFLAGS) -Ddlsym=s3e_input_test_dlsym \
	  -Ddlclose=s3e_input_test_dlclose $(HOST_TEST_CFLAGS) $(PROJECT_CFLAGS) \
	  -o $@ tests/s3e_input_test.c src/s3e_input.c $(HOST_TEST_LDFLAGS)

.PHONY: clean format format-check test test-sanitize
