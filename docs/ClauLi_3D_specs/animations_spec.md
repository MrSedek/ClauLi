# EMO-style глаза для ESP32 — полная анимационная система

## Архитектурный принцип

Каждый глаз — это структура из 8 параметров. **Все 25 анимаций — это просто разные значения этих параметров во времени.** Никаких спрайтов, никаких bitmap-ассетов.

```cpp
struct EyeFrame {
    int16_t  cx, cy;          // центр глаза
    int16_t  width, height;   // размеры
    int16_t  radius;          // скругление углов
    int16_t  tilt;            // наклон в градусах ±45
    uint16_t color;           // RGB565
    uint8_t  top_cut;         // срез сверху (для бровей) 0-255
    uint8_t  bot_cut;         // срез снизу (для грустных век)
    uint8_t  cut_direction;   // 0=inner, 1=outer (куда наклон среза)
};

struct FaceState {
    EyeFrame left;
    EyeFrame right;
    // оверлеи
    bool show_tear;
    bool show_blush;
    bool show_sparkle;
    char overlay_symbol;      // '!', '?', 'Z', 0=нет
};
```

## Базовая геометрия (idle / neutral) для 320×240

```
left.cx  = 125    left.cy  = 115
right.cx = 235    right.cy = 115
width    = 66     height   = 70
radius   = 22     tilt     = 0
color    = 0x6E7A (#5DD5D5)
top_cut  = 0      bot_cut  = 0
```

## Каталог 25 анимаций

### Категория 1: Idle (базовые движения)

| # | Имя | Длительность | Параметры | Триггер |
|---|-----|--------------|-----------|---------|
| 1 | `blink` | 200ms | height: 70→2→70 (asymmetric ease) | каждые 3-6s |
| 2 | `wink` | 400ms | only right: height 70→2→70 (slow) | manual |
| 3 | `saccade` | 600ms | both cx: ±10-14px, perspective scale | каждые 8-15s |
| 4 | `breathing` | 4000ms loop | scale: 0.98↔1.02 (sine) | постоянно |
| 5 | `double_blink` | 380ms | blink × 2 quick | manual / pre-emotion |
| 6 | `look_around` | 1500ms | cx/cy: 8 directions sequentially | каждые 30-60s |
| 7 | `shake_no` | 500ms | cx: ±7px @ 6Hz | manual |
| 8 | `nod_yes` | 600ms | cy: ±6px @ 3Hz | manual |
| 9 | `squint` | 300ms | height: 70→28, top_cut+bot_cut | pre-focus |

### Категория 2: Emotions (выраженные состояния)

| # | Имя | Длительность | Ключевые параметры |
|---|-----|--------------|-----|
| 10 | `happy` | 500ms | shape→arc smile + blush overlay |
| 11 | `sad` | 800ms | top_cut outer + tear overlay + slow |
| 12 | `angry` | 400ms | top_cut inner + color→#FF5577 + shake |
| 13 | `suspicious` | 600ms | **АСИММ:** L squint, R saccade |
| 14 | `love` | 700ms | shape→heart + color→#FF5577 + sparkles |
| 15 | `surprised` | 250ms | rapid scale 1.0→1.3→1.0 + jump up + '!' |
| 16 | `sleepy` | 2000ms | height→0 with jerks + 'Z' overlay |
| 17 | `curious` | 800ms | tilt+12° + L grow + '?' |
| 18 | `confused` | 900ms | **АСИММ:** L cx-8, R cx+8 + multi '?' |

### Категория 3: Reactive (системные события)

| # | Имя | Длительность | Параметры | Когда играть |
|---|-----|--------------|-----------|------|
| 19 | `glitch` | 400ms | RGB split + scanline + jitter | network drop, error |
| 20 | `scan` | 1200ms loop | overlay vertical bar L→R | processing, loading |
| 21 | `success` | 600ms | color→green + shape→checkmark + sparkle | CI passed |
| 22 | `error` | 500ms | shake + shape→X + color→red | CI failed |
| 23 | `progress` | variable | inner fill bar 0-100% | builds, downloads |
| 24 | `alert` | 800ms loop | color→amber + pulse + '!' | notifications |
| 25 | `boot` | 1000ms | dot→line→half→full→hi! | startup |

## Easing functions

Не использовать линейную интерполяцию — выглядит как робот. Использовать:

```cpp
// быстрый старт, медленное завершение (для blink open, sad)
float easeOutCubic(float t) { return 1 - pow(1 - t, 3); }

// медленный старт, быстрое завершение (для blink close, surprised)
float easeInCubic(float t) { return t * t * t; }

// плавно туда-обратно (для всех симметричных движений)
float easeInOutCubic(float t) {
    return t < 0.5 ? 4*t*t*t : 1 - pow(-2*t + 2, 3) / 2;
}

// упругий (для surprised, success)
float easeOutBack(float t) {
    float c1 = 1.70158, c3 = c1 + 1;
    return 1 + c3 * pow(t - 1, 3) + c1 * pow(t - 1, 2);
}
```

## Composition rules (важно!)

1. **Idle никогда не выключается.** breathing работает всегда, даже во время эмоций. Просто amplitude меняется (в sad/sleepy — медленнее; в excited/love — быстрее).

2. **Blink и saccade перебиваются** другими анимациями, но запускаются заново после.

3. **Эмоции одна за другой через transition.** Прямой jump между happy и sad выглядит ужасно. Всегда interpolate с длительностью 200-400ms.

4. **Reactive перебивают всё.** error/glitch/alert имеют приоритет над эмоциями и idle. После их окончания — возврат в предыдущую эмоцию.

5. **Pre-roll.** Сильные эмоции (happy, surprised, love) лучше начинать с быстрого double_blink — это даёт ощущение «осознания».

## State machine

```
                    ┌──────────┐
                    │   BOOT   │ (one-shot, 1000ms)
                    └─────┬────┘
                          ▼
                    ┌──────────┐
        ┌──────────►│   IDLE   │◄──────────┐
        │           └─────┬────┘            │
        │                 │                 │
        │  (timer/event)  │                 │ (transition_end)
        │                 ▼                 │
        │           ┌──────────┐            │
        │           │ EMOTION  │            │
        │           │  (hold)  │────────────┤
        │           └─────┬────┘            │
        │                 │                 │
        │   (event)       ▼                 │
        │           ┌──────────┐            │
        └───────────│ REACTIVE │────────────┘
                    │  (one)   │
                    └──────────┘
```

В коде:
```cpp
enum class State { BOOT, IDLE, EMOTION, REACTIVE };
enum class Emotion { NEUTRAL, HAPPY, SAD, ANGRY, SUSPICIOUS, LOVE, 
                     SURPRISED, SLEEPY, CURIOUS, CONFUSED };
enum class Reactive { NONE, GLITCH, SCAN, SUCCESS, ERROR_, PROGRESS, ALERT };
```

## Расширенный промпт для Claude

Скопируй ниже в новый чат с Claude (лучше — в Claude Code):

---

```
Контекст: ESP32 + дисплей 320×240 IPS (драйвер: УТОЧНЮ при общении) + LVGL 9 + 
Arduino GFX Library (moononournation). Делаю анимированного персонажа в стиле 
робота EMO от Living.AI — два бирюзовых глаза-прямоугольника на чёрном фоне.

Я хочу полную систему анимации из 25 движений. Архитектура должна быть такой, 
чтобы добавление новой анимации требовало написания только одной функции 
"что я хочу видеть в момент времени t от 0 до 1".

КЛЮЧЕВЫЕ ТРЕБОВАНИЯ:

1. Все 25 анимаций строятся на одной структуре EyeFrame (см. ниже). Никаких 
   спрайтов или PNG-ассетов. Только параметры + рендеринг примитивами.

2. struct EyeFrame:
   int16_t cx, cy, width, height, radius, tilt;
   uint16_t color;
   uint8_t top_cut, bot_cut, cut_direction;
   
   struct FaceState { EyeFrame left, right; уведомления-оверлеи };

3. Базовая геометрия (idle/neutral):
   left.cx=125, right.cx=235, cy=115, width=66, height=70, radius=22,
   tilt=0, color=0x6E7A (#5DD5D5)

4. КАТЕГОРИИ АНИМАЦИЙ:
   
   IDLE (всегда активны, перебиваются): blink, wink, saccade, breathing, 
   double_blink, look_around, shake_no, nod_yes, squint
   
   EMOTIONS (запускаются по триггеру, удерживаются): happy, sad, angry, 
   suspicious, love, surprised, sleepy, curious, confused
   
   REACTIVE (приоритет над всем, one-shot): glitch, scan, success, error, 
   progress, alert, boot
   
   Конкретные параметры каждой анимации я предоставлю — но сначала покажи 
   архитектуру.

5. ЛЕРПИНГ + EASING. Между состояниями интерполируй параметры EyeFrame с 
   easeInOutCubic (или специальным easing для конкретной анимации). 
   Не линейно — линейное выглядит как робот.

6. КОМПОЗИЦИЯ:
   - Idle никогда не выключается, breathing работает поверх эмоций
   - Эмоции переходят друг в друга через 200-400ms transition
   - Reactive имеет приоритет, после неё возврат в предыдущую эмоцию
   - Pre-roll: сильные эмоции начинаются с double_blink

7. РЕНДЕРИНГ: GFX Library Canvas (off-screen 320×240×16бит = 153КБ) + DMA push. 
   Прямая отрисовка на дисплей даст тиринг.
   
   Для top_cut/bot_cut используй маску — нарисуй полный rounded_rect, потом 
   нарисуй чёрный треугольник поверх. Это дешевле, чем custom shape.

8. ЭФФЕКТ СВЕЧЕНИЯ: каждый глаз рисуется в 2 слоя — основной цвет внизу 
   (#5DD5D5), highlight сверху смещённый на 3px вверх (#7DEEEE). Имитирует 
   CRT-свечение, делает «премиум»-вид.

ЧТО Я ХОЧУ ОТ ТЕБЯ:

Шаг 1: предложи архитектурный план — какие классы, как работает state machine, 
как организован renderer, как ведёт себя event-loop в loop(). Не код, а схему. 
После моего ОК — пиши код.

Шаг 2: полный .ino файл с этой архитектурой + минимум 5 анимаций готово 
(blink, saccade, happy, sad, glitch) + комментарии-болванки для остальных 20 
с указанием параметров.

Шаг 3: пример как добавить НОВУЮ анимацию (для понимания паттерна) — например 
покажи «winks twice and then blushes» как комбинацию.

Спроси у меня, какой драйвер дисплея (ILI9341/ST7789/ILI9488) и как подключён, 
прежде чем писать код, который зависит от железа. Не угадывай.

Я приложу к этому промпту референсы EMO в виде GIF и спецификацию анимаций 
(см. файл animations_spec.md).
```

---

## Что прикладывать к промпту

1. Этот файл (полная спецификация 25 анимаций)
2. Файл `emo_eyes_prompt.md` из предыдущего шага (базовая палитра, размеры)
3. GIF-референсы EMO, которые ты уже мне показывал
4. **Распиновку и драйвер твоего дисплея** — это критично, без этого Claude угадает неправильно

## Производительность

Бюджет на ESP32 (240 МГц):
- 1 кадр на 30 FPS = 33ms
- Из них: ~10ms на DMA-push на дисплей (SPI 40 МГц)
- ~5ms на canvas clear + draw eyes
- ~3ms на оверлеи и easing math
- Остаётся 15ms на всё остальное (WiFi, sensors, и т.д.)

Запас приличный. Реалистично выйти на 40-50 FPS, если не используешь LVGL для самих глаз (а только для оверлеев).

## Идеи композитных анимаций

После того, как 25 базовых работают, легко собирать сложные сцены:

- **«Hello»** = boot → double_blink → happy → wink
- **«CI failed but recovering»** = error → sad → scan → curious → happy
- **«Network down»** = alert → glitch (looped) → scan
- **«Pomodoro break»** = squint → sleepy → break_overlay → boot (re-wake)
- **«Mocking a colleague»** = neutral → suspicious → wink → happy

Это и есть «жизнь» персонажа — комбинации, а не отдельные кадры.
