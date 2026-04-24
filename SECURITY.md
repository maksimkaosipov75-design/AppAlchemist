# Security Policy

AppAlchemist processes third-party Linux packages and archives. Treat input packages as untrusted.

## Reporting A Vulnerability

Please open a private security advisory on GitHub, or contact the maintainer through the repository profile.

## Safety Notes

- Do not run generated AppImages from untrusted packages unless you understand the source of the package.
- Inspect conversion logs and generated AppDir contents before sharing AppImages publicly.
- Prefer testing generated AppImages in a disposable user account, VM, or container when the source package is unknown.
