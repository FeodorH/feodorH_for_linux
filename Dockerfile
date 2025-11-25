FROM ubuntu:22.04

# Установка системных зависимостей
RUN apt-get update && apt-get install -y \
    build-essential \
    g++ \
    cmake \
    make \
    autoconf \
    automake \
    libtool \
    pkg-config \
    python3 \
    python3-pip \
    python3-pytest \
    git \
    && rm -rf /var/lib/apt/lists/*

# Копируем тесты из оригинального контейнера (или устанавливаем их)
# Если тесты доступны как pip пакет или из git
RUN pip3 install pytest

# Создаем рабочую директорию
WORKDIR /workspace

# Копируем скрипт для запуска тестов
COPY run_tests.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/run_tests.sh

CMD ["/bin/bash"]
