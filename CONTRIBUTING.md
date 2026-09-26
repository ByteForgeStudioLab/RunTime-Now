# Contributing to RunTime-Now

🇬🇧 [English](#english) · 🇺🇿 [O'zbekcha](#ozbekcha)

## English

Thanks for your interest! Issues and pull requests are welcome.

### Setup

```sh
git clone --recursive <repo-url> RunTime-Now
cd RunTime-Now
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
tests/run.sh
```

### Before opening a pull request

1. `tests/run.sh` passes (it needs `python3` for the HTTP tests).
2. New behaviour has a test: a file in `tests/cases/` with its expected `.out`
   (and `.exit` for a non-zero exit code), a `tests/strip/*.ts` case for the
   TypeScript stripper, or a check in `tests/http_test.py`.
   `tests/run.sh --update` regenerates expected outputs — review the diff before committing.
3. No compiler warnings with both GCC and Clang (`-Wall -Wextra` is on).
4. Memory stays clean: `valgrind --leak-check=full build/rtn your-test.js`
   (a Debug build also makes QuickJS assert on leaked JS objects at exit).

### Code style

- C++20, 4-space indentation, `snake_case` functions, `PascalCase` types.
- Comments explain *why*; keep them short.
- Where Node, Deno and Bun agree on an API's behaviour, match it.

## O'zbekcha

Qiziqishingiz uchun rahmat! Issue va pull request'lar qabul qilinadi.

### Sozlash

```sh
git clone --recursive <repo-url> RunTime-Now
cd RunTime-Now
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
tests/run.sh
```

### Pull request ochishdan oldin

1. `tests/run.sh` muvaffaqiyatli o'tsin (HTTP testlar uchun `python3` kerak).
2. Yangi xatti-harakat uchun test qo'shing: `tests/cases/` ga fayl va kutilgan `.out`
   (nol bo'lmagan chiqish kodi uchun `.exit`), TypeScript stripper uchun
   `tests/strip/*.ts`, yoki `tests/http_test.py` ga tekshiruv.
   `tests/run.sh --update` kutilgan natijalarni qayta yozadi — commit qilishdan oldin diff'ni ko'rib chiqing.
3. GCC va Clang'da bitta ham ogohlantirish bo'lmasin (`-Wall -Wextra` yoqilgan).
4. Xotira toza bo'lsin: `valgrind --leak-check=full build/rtn test.js`
   (Debug build'da QuickJS chiqishda oqib qolgan JS obyektlarini ham tekshiradi).

### Kod uslubi

- C++20, 4 bo'shliq chekinish, funksiyalar `snake_case`, turlar `PascalCase`.
- Izohlar *nima uchun* ekanini tushuntirsin va qisqa bo'lsin.
- Node, Deno va Bun bir xil ishlaydigan API'larda ularga moslang.
