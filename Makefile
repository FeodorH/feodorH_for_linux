all: build package

build:
	cmake -B build .
	cmake --build build

run: build
	./build/kubsh

package: build
	mkdir -p kubsh-package/DEBIAN
	mkdir -p kubsh-package/usr/bin
	cp build/kubsh kubsh-package/usr/bin/
	chmod +x kubsh-package/usr/bin/kubsh
	echo "Package: kubsh" > kubsh-package/DEBIAN/control
	echo "Version: 1.0" >> kubsh-package/DEBIAN/control
	echo "Section: utils" >> kubsh-package/DEBIAN/control
	echo "Priority: optional" >> kubsh-package/DEBIAN/control
	echo "Architecture: amd64" >> kubsh-package/DEBIAN/control
	echo "Maintainer: Mitohondriyaa" >> kubsh-package/DEBIAN/control
	echo "Description: Kubsh" >> kubsh-package/DEBIAN/control
	dpkg-deb --build kubsh-package
	mv kubsh-package.deb kubsh.deb

clean:
	rm -rf build kubsh.deb kubsh-package

.PHONY: build run package clean
