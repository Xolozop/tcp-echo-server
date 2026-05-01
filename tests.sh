#!/bin/bash

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

PORT=8080
HOST="localhost"

# Проверка наличия nc
if ! command -v nc &> /dev/null; then
    echo -e "${RED}Error: 'nc' (netcat) not found!${NC}"
    echo -e "${YELLOW}Install with: sudo pacman -S openbsd-netcat${NC}"
    exit 1
fi

# Проверка существования бинарника
if [ ! -f "./echo_zero_copy" ]; then
    echo -e "${RED}Error: echo_zero_copy not found! Run 'make' first.${NC}"
    exit 1
fi

# Функция для освобождения порта
free_port() {
    sudo fuser -k $PORT/tcp 2>/dev/null || true
    sleep 1
}

# Заголовок
echo -e "${BLUE}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║         ZERO-COPY ECHO SERVER TEST                       ║${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Освобождаем порт
free_port

# Запуск сервера
echo -e "${YELLOW}Starting Zero-Copy server...${NC}"
./echo_zero_copy $PORT > /tmp/zero_copy_test.log 2>&1 &
SERVER_PID=$!
sleep 2

# Проверка запуска
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${RED}Server failed to start!${NC}"
    cat /tmp/zero_copy_test.log
    exit 1
fi
echo -e "${GREEN}Server started (PID: $SERVER_PID)${NC}"
echo ""

PASSED=0
FAILED=0

# Тест 1: Базовое эхо
echo -n "Test 1: Basic echo... "
RESULT=$(echo "Hello" | timeout 2 nc $HOST $PORT 2>/dev/null)
if [ "$RESULT" = "Hello" ]; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    ((FAILED++))
fi

# Тест 2: Множественные сообщения
echo -n "Test 2: Multiple messages... "
RESULT=$(echo -e "A\nB\nC" | timeout 2 nc $HOST $PORT 2>/dev/null)
if echo "$RESULT" | grep -q "B"; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    ((FAILED++))
fi

# Тест 3: Среднее сообщение (1KB)
echo -n "Test 3: Medium message (1KB)... "
MSG=$(dd if=/dev/zero bs=1024 count=1 2>/dev/null | tr '\0' 'X')
RESULT=$(echo "$MSG" | timeout 2 nc $HOST $PORT 2>/dev/null)
if [ "$RESULT" = "$MSG" ]; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    ((FAILED++))
fi

# Тест 4: Большое сообщение (64KB)
echo -n "Test 4: Large message (64KB)... "
MSG=$(dd if=/dev/zero bs=1024 count=64 2>/dev/null | tr '\0' 'X')
RESULT=$(echo "$MSG" | timeout 3 nc $HOST $PORT 2>/dev/null)
if [ "$RESULT" = "$MSG" ]; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED (size mismatch)${NC}"
    ((FAILED++))
fi

# Тест 5: 10 конкурентных соединений
echo -n "Test 5: 10 concurrent connections... "
TEMP_FILE="/tmp/concurrent_$$.txt"
> "$TEMP_FILE"  # Очистка файла

# Массив для хранения PID
PIDS=()

for i in {1..10}; do
    (
        RESULT=$(echo "Msg$i" | timeout 2 nc $HOST $PORT 2>/dev/null)
        echo "$RESULT" >> "$TEMP_FILE"
    ) &
    PIDS+=($!)
done

# Ждём все процессы с таймаутом
TIMEOUT=10
WAITED=0
for pid in "${PIDS[@]}"; do
    wait $pid 2>/dev/null
done

# Считаем результат
COUNT=$(grep -c "Msg" "$TEMP_FILE" 2>/dev/null)
rm -f "$TEMP_FILE"

if [ "$COUNT" -eq 10 ]; then
    echo -e "${GREEN}PASSED (10/10)${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED ($COUNT/10)${NC}"
    ((FAILED++))
fi

# Тест 6: Длительное соединение
echo -n "Test 6: Persistent connection... "
RESULT=$( { echo "First"; sleep 1; echo "Second"; sleep 1; echo "Third"; } | timeout 5 nc $HOST $PORT 2>/dev/null )
if echo "$RESULT" | grep -q "Second"; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    ((FAILED++))
fi

# Тест 7: Специальные символы
echo -n "Test 7: Special characters... "
RESULT=$(echo "!@#$%^&*()_+" | timeout 2 nc $HOST $PORT 2>/dev/null)
if [ "$RESULT" = "!@#$%^&*()_+" ]; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    ((FAILED++))
fi

# Тест 8: 10 последовательных соединений
echo -n "Test 8: 10 sequential connections... "
COUNT=0
for i in {1..10}; do
    RESULT=$(timeout 1 nc $HOST $PORT <<< "Test$i" 2>/dev/null)
    if [ "$RESULT" = "Test$i" ]; then
        ((COUNT++))
    else
        echo -n ""
    fi
    # Небольшая задержка между соединениями
    sleep 0.05
done

if [ $COUNT -eq 10 ]; then
    echo -e "${GREEN}PASSED (10/10)${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED ($COUNT/10)${NC}"
    ((FAILED++))
fi

# Остановка сервера
echo ""
echo -e "${YELLOW}Stopping server...${NC}"
kill -TERM $SERVER_PID 2>/dev/null
sleep 1
free_port

# Результаты
echo ""
echo -e "${BLUE}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║                    TEST RESULTS                         ║${NC}"
echo -e "${BLUE}╠══════════════════════════════════════════════════════════╣${NC}"
echo -e "${BLUE}║ Passed:        ${PASSED}${NC}"
echo -e "${RED}║ Failed:        ${FAILED}${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════════════════╝${NC}"

if [ $FAILED -eq 0 ]; then
    echo -e "\n${GREEN}ALL TESTS PASSED${NC}"
    exit 0
else
    echo -e "\n${RED}$FAILED TESTS FAILED${NC}"
    exit 1
fi