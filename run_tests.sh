#!/bin/bash

set -e

echo "=== Building shell ==="
cd /workspace

# Проверяем систему сборки и компилируем
if [ -f "configure" ]; then
    ./configure
elif [ -f "configure.ac" ]; then
    autoreconf -i
    ./configure
elif [ -f "CMakeLists.txt" ]; then
    mkdir -p build && cd build
    cmake ..
else
    echo "Using existing Makefile"
fi

# Компилируем
make

# Создаем deb-пакет если есть соответствующая цель
if make -n deb 2>/dev/null; then
    make deb
fi

echo "=== Installing and testing ==="
# Устанавливаем пакет
if [ -f "kubsh.deb" ]; then
    apt-get update
    apt-get install -y ./kubsh.deb
elif [ -f "build/kubsh.deb" ]; then
    apt-get install -y ./build/kubsh.deb
fi

# Запускаем тесты
cd /opt
if [ -d "tests" ]; then
    pytest -v --log-cli-level=10
else
    echo "Tests not found in /opt/tests"
    # Ищем тесты в других местах
    find / -name "*test*" -type d | grep -E "(test|tests)" | head -5
fi
