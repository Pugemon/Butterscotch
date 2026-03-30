#!/bin/bash

echo "=== Начало конвертации PNG → T3X ==="

count=0
total=$(ls exported_linear/*.png 2>/dev/null | wc -l)

echo "Найдено файлов: $total"
echo
start_time=$(date +%s)
for file in exported_linear/*.png; do
    ((count++))

    filename=$(basename "$file" .png)
    output="t3x/${filename}.t3x"

    echo "[$count/$total] Обработка: $file"
    echo " -> Выход: $output"

    if tex3ds -f etc1a4 -z none -o "$output" "$file"; then
        echo " ✔ Успешно"
    else
        echo " ✖ Ошибка при обработке $file"
    fi

    echo "-----------------------------------"
done

echo
echo "=== Готово: обработано $count файлов ==="
end_time=$(date +%s)
echo "Время выполнения: $((end_time - start_time)) сек"