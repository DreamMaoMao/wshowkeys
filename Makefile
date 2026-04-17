.PHONY: all clean install

# Default target: build the project
all: build/build.ninja
	ninja -C build

# Configure the build directory if it doesn't exist
build/build.ninja:
	meson setup build

# Install the project and set the required root permissions
install: all
	ninja -C build install
	chmod a+s /usr/local/bin/wshowkeys

# Uninstall the binary from the system
uninstall:
	rm -f /usr/local/bin/wshowkeys

# Clean up by removing the build directory
clean:
	rm -rf build
