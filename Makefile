CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pedantic -pthread
TARGET = kubsh
SRC = main.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC)

run: $(TARGET)
	./$(TARGET)

deb: $(TARGET)
	@echo "Building Debian package..."
	rm -rf deb_build
	mkdir -p deb_build/kubsh/DEBIAN
	mkdir -p deb_build/kubsh/usr/bin
	cp kubsh deb_build/kubsh/usr/bin/
	@echo "Package: kubsh" > deb_build/kubsh/DEBIAN/control
	@echo "Version: 1.0-1" >> deb_build/kubsh/DEBIAN/control
	@echo "Architecture: amd64" >> deb_build/kubsh/DEBIAN/control
	@echo "Maintainer: Feodor <main_user@example.com>" >> deb_build/kubsh/DEBIAN/control
	@echo "Depends: libc6, adduser" >> deb_build/kubsh/DEBIAN/control  # Добавил adduser
	@echo "Section: utils" >> deb_build/kubsh/DEBIAN/control
	@echo "Priority: optional" >> deb_build/kubsh/DEBIAN/control
	@echo "Description: Custom shell with user management VFS" >> deb_build/kubsh/DEBIAN/control
	@echo " A custom shell implementation that provides virtual file system" >> deb_build/kubsh/DEBIAN/control
	@echo " for user management with automatic directory monitoring." >> deb_build/kubsh/DEBIAN/control
	@echo "#!/bin/sh" > deb_build/kubsh/DEBIAN/postinst
	@echo "set -e" >> deb_build/kubsh/DEBIAN/postinst
	@echo "# Set setuid bit so kubsh can run as root for user management" >> deb_build/kubsh/DEBIAN/postinst
	@echo "chown root:root /usr/bin/kubsh" >> deb_build/kubsh/DEBIAN/postinst
	@echo "chmod 4755 /usr/bin/kubsh" >> deb_build/kubsh/DEBIAN/postinst
	@echo "echo 'kubsh 1.0 installed successfully!'" >> deb_build/kubsh/DEBIAN/postinst
	@echo "echo 'Run \"kubsh\" to start the custom shell.'" >> deb_build/kubsh/DEBIAN/postinst
	chmod 755 deb_build/kubsh/DEBIAN/postinst
	dpkg-deb --build deb_build/kubsh kubsh.deb
	rm -rf deb_build
	@echo "✅ Debian package created: kubsh.deb"

clean:
	rm -f $(TARGET)
	rm -f kubsh.deb
	rm -rf deb_build

.PHONY: all run deb clean
