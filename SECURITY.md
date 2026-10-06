# Credentials and local data

Do not put Wi-Fi passwords, Hubspace passwords, refresh/access tokens, or account exports in an issue, commit, or pull request.

The setup tools prompt locally and send configuration over USB. They do not read saved Windows Wi-Fi credentials and do not save cloud credentials to disk. The ESP32 stores its Wi-Fi password, refresh token, account ID, enabled flag, and BLE counter in unencrypted NVS. USB physical access can reconfigure the device. This is a home prototype, not a hardened security appliance.

`include/bridge_config.h` contains local device/controller identities and is ignored. `.local/`, captures, logs, binary images, and encrypted session files are also ignored. Ignore rules are not a substitute for checking staged files before publication. Do not upload full flash backups of a provisioned board. Before transferring a board, erase its flash with the appropriate Espressif tooling; this also removes the application and configuration.

TLS certificate and hostname checks are enabled. Public root certificates in `src/trust.h` are not private credentials. Their maintenance and unofficial API changes remain the operator's responsibility.

For a security issue, initially describe the issue without credentials, household data, or exploit payloads. Coordinate privately with the repository owner if sensitive details are necessary. There is no promise of a security-response SLA.
