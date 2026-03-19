ИНСТРУКЦИЯ ПО СБОРКЕ AppAlchemist ARM64

Для сборки ARM64 версии AppAlchemist на вашей системе Asahi Linux (M1 Mac):

1. Убедитесь, что у вас установлены зависимости:
   sudo pacman -S base-devel cmake qt6-base qt6-tools wget

2. Перейдите в директорию проекта:
   cd /path/to/deb-to-appimage

3. Запустите скрипт сборки:
   ./packaging/build-appimage-arm64.sh

4. Готовый AppImage будет создан в:
   releases/AppAlchemist-ARM64.AppImage

ВАЖНО: Этот скрипт должен запускаться на ARM64 системе (Asahi Linux).
На x86_64 системе он соберет x86_64 версию, а не ARM64.

Для автоматической сборки используйте GitHub Actions workflow:
.github/workflows/build-arm64.yml
