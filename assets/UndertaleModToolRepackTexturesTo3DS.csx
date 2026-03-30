using System;
using System.Linq;
using System.IO;
using System.Threading.Tasks;
using UndertaleModLib.Scripting;
using UndertaleModLib.Models;
using ImageMagick;
using UndertaleModLib.Util;

EnsureDataLoaded();

// Функция для поиска ближайшей степени двойки (минимум 8)
int GetNextPOT(int value)
{
    if (value < 8) return 8;
    int pot = 1;
    while (pot < value) pot <<= 1;
    return pot;
}

volatile int progress = 0;
string updateText = "";
void UpdateProgress(int updateAmount)
{
    UpdateProgressBar(null, updateText, progress += updateAmount, Data.TexturePageItems.Count);
}

void ResetProgress(string text)
{
    progress = 0;
    updateText = text;
    UpdateProgress(0);
}

// 1. ИСПРАВЛЕНИЕ: Ставим 1024, чтобы избежать сжатия фонов и артефактов
int maxPOT = 1024;
string packagerDirectory = Path.Combine(ExePath, "Packager");
Directory.CreateDirectory(packagerDirectory);

ResetProgress("Fixing Font Coordinates...");

// 2. ИСПРАВЛЕНИЕ: Перевод координат шрифтов из абсолютных в локальные!
foreach (var font in Data.Fonts)
{
    if (font.Texture == null) continue;
    var tpag = font.Texture;

    foreach (var glyph in font.Glyphs)
    {
        // Смещаем глифы относительно начала их нового обрезанного атласа
        if (glyph.SourceX >= tpag.SourceX)
            glyph.SourceX = (ushort)(glyph.SourceX - tpag.SourceX);
        if (glyph.SourceY >= tpag.SourceY)
            glyph.SourceY = (ushort)(glyph.SourceY - tpag.SourceY);
    }
}

ResetProgress("Processing POT Textures...");

Data.EmbeddedTextures.Clear();
if (Data.TextureGroupInfo != null)
{
    foreach (var texInfo in Data.TextureGroupInfo)
    {
        if (texInfo is null) continue;
        texInfo.TexturePages.Clear();
    }
}

SyncBinding("EmbeddedTextures", true);

await Task.Run(() =>
{
    using var worker = new TextureWorker();

    for (int i = 0; i < Data.TexturePageItems.Count; i++)
    {
        var item = Data.TexturePageItems[i];

        // 1. Получаем исходный размер
        int originalW = item.SourceWidth;
        int originalH = item.SourceHeight;

        // ИСПРАВЛЕНИЕ: Сохраняем оригинальные параметры обрезки!
        // 2. Если спрайт больше 512, масштабируем его до вписывания в 512
        ushort oldTargetX = item.TargetX;
        ushort oldTargetY = item.TargetY;
        ushort oldTargetW = item.TargetWidth;
        ushort oldTargetH = item.TargetHeight;
        ushort oldBoundingW = item.BoundingWidth;
        ushort oldBoundingH = item.BoundingHeight;

        float scale = 1.0f;
        if (originalW > maxPOT || originalH > maxPOT)
        {
            scale = Math.Min((float)maxPOT / originalW, (float)maxPOT / originalH);
            originalW = Math.Max(1, (int)(originalW * scale));
            originalH = Math.Max(1, (int)(originalH * scale));
        }

        // 3. Вычисляем размер POT-холста (минимум 8x8)
        int potW = GetNextPOT(originalW);
        int potH = GetNextPOT(originalH);

        // Формируем имя по запросу: s{POT_resolution}_page_{index}.png
        string resolutionStr = $"{potW}x{potH}";
        string filename = $"s{resolutionStr}_page_{i}.png";
        string fullPath = Path.Combine(packagerDirectory, filename);

        // 4. Создаем POT-холст и помещаем туда спрайт
        using (MagickImage canvas = new MagickImage(MagickColors.Transparent, (uint)potW, (uint)potH))
        {
            // Извлекаем временный PNG
            string tempSprite = Path.Combine(packagerDirectory, $"temp_{i}.png");
            worker.ExportAsPNG(item, tempSprite);

            using (MagickImage spriteImg = new MagickImage(tempSprite))
            {
                // Если было масштабирование (больше 1024), ресайзим сам спрайт
                if (originalW != item.SourceWidth || originalH != item.SourceHeight)
                    spriteImg.Resize((uint)originalW, (uint)originalH);

                // Рисуем спрайт в угол 0,0
                canvas.Composite(spriteImg, 0, 0, CompositeOperator.Copy);
            }

            // Сохраняем итоговый файл
            canvas.Write(fullPath);
            File.Delete(tempSprite);

            // 5. Регистрируем текстуру в игре
            UndertaleEmbeddedTexture tex = new UndertaleEmbeddedTexture();
            tex.Scaled = item.TexturePage?.Scaled ?? 0;
            tex.TextureData.Image = GMImage.FromMagickImage(canvas).ConvertToPng();
            Data.EmbeddedTextures.Add(tex);

            // 6. Обновляем данные элемента (важно: SourceWidth/Height — это РЕАЛЬНЫЙ размер внутри POT)
            item.TexturePage = tex;
            item.SourceX = 0;
            item.SourceY = 0;
            item.SourceWidth = (ushort)originalW;
            item.SourceHeight = (ushort)originalH;

            // Возвращаем Target-координаты обратно с учетом масштаба
            item.TargetX = (ushort)Math.Round(oldTargetX * scale);
            item.TargetY = (ushort)Math.Round(oldTargetY * scale);
            item.TargetWidth = (ushort)Math.Round(oldTargetW * scale);
            item.TargetHeight = (ushort)Math.Round(oldTargetH * scale);
            item.BoundingWidth = (ushort)Math.Round(oldBoundingW * scale);
            item.BoundingHeight = (ushort)Math.Round(oldBoundingH * scale);
        }

        UpdateProgress(1);
    }
});

DisableAllSyncBindings();
HideProgressBar();

ScriptMessage("Готово! Все текстуры приведены к POT и сохранены отдельно.");