# MP2Assist

Трейнер и запускатор для одиночной кампании **Max Payne 2 (RU)** в Steam. Поддерживается русская версия Steam с App ID `12260`, Build ID `2880` и `Max Payne 2 v1.01 / Build 102`. При запуске проверяются контрольные суммы файлов этой сборки.

## Установка и запуск

1. Скачай `MP2Assist-v1.0.0-win32.zip` в [Releases](https://github.com/wericool/max-payne-2-assist/releases) и распакуй его в отдельную папку с правом записи. Оставь `MP2Assist.exe` и `MP2Assist.dll` рядом.
2. Открой `MP2Assist.exe`. Он ищет игру в библиотеках Steam.
3. Для обычного запуска нажми **Установить Widescreen Fix**, выбери разрешение и режим экрана, затем нажми **Играть через Steam**. По умолчанию выбран режим 3440×1440 без рамки. Установка фикса требует интернета и системных `curl.exe` и `tar.exe` в Windows.
4. Для режима тренера нажми **Ждать запуска** и запусти игру через Steam. Для этого режима Widescreen Fix не требуется.

## Управление в игре

| Клавиша | Действие |
| --- | --- |
| Insert | Открыть или закрыть меню |
| Мышь | Переключать пункты и перетаскивать ползунок дальности |
| PgUp / PgDn | Выбирать пункт |
| Home / End | Менять значение выбранного пункта |
| F6 | Приостановить или возобновить весь ESP |
| F7 | Переключить отключение разброса |

Рамки, кости, контур игрового хитбокса головы и полоска HP включаются независимо. Дальность всего ESP считается от игрока. Доступны 5, 10, 20, 30, 50, 70, 100 м и режим **Вся карта** без ограничения дальности среди обрабатываемых игрой целей. ESP скрывает цель при начале смерти. Настройки меню хранятся в `MP2Assist.ini` рядом с запускатором.

## Сборка

Требуются Visual Studio 2022 или Build Tools с компонентами **Desktop development with C++**, компилятором MSVC x86 и Windows SDK. Запусти `build.cmd`. Готовые `MP2Assist.exe` и `MP2Assist.dll` появятся в папке `release`.

Для проверки найденной сборки без запуска игры используй `MP2Assist.exe --verify`. Для другого расположения того же издания можно передать `--game "путь к папке игры"` вместе с `--verify`, `--wait` или `--install-fix`.

## Авторы Widescreen Fix

Наработки и основа широкоформатного режима взяты из **Max Payne 2 Widescreen Fix** от **ThirteenAG** и участников WidescreenFixesPack. Кнопка установки скачивает оригинальный архив с GitHub, сверяет SHA-256 и копирует необходимые компоненты. В том числе используются Ultimate ASI Loader от ThirteenAG и DxWrapper от Elisha Riedlinger.

- [Оригинальный релиз Max Payne 2 Widescreen Fix](https://github.com/ThirteenAG/WidescreenFixesPack/releases/tag/mp2)
- [Исходники и лицензия WidescreenFixesPack](https://github.com/ThirteenAG/WidescreenFixesPack/tree/mp2)
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
- [DxWrapper](https://github.com/elishacloud/dxwrapper)
