/* tls.h - verschluesselte Verbindungen nach TLS 1.3.
 *
 * Unterstuetzt wird ausschliesslich TLS 1.3 mit dem Schluesseltausch
 * ueber X25519 und den Verfahren AES-128-GCM oder ChaCha20-Poly1305.
 * Aeltere Fassungen werden abgelehnt - sie haben bekannte Schwaechen,
 * und ein System, das sie noch spricht, laedt zum Herunterstufen ein.
 */
#ifndef TLS_H
#define TLS_H

#include "retro.h"

struct tcp_socket;
struct tls_connection;

/* Baut auf einer bestehenden TCP-Verbindung die Verschluesselung auf.
 * Der Rechnername wird fuer die Namensangabe und die Pruefung des
 * Zertifikats gebraucht. */
struct tls_connection *tls_connect(struct tcp_socket *socket, const char *host,
                                   char *error, size_t error_size);

int  tls_send(struct tls_connection *tls, const void *data, uint32_t length);
int  tls_receive(struct tls_connection *tls, void *buffer, uint32_t capacity,
                 uint32_t timeout_ms);
bool tls_finished(const struct tls_connection *tls);
void tls_close(struct tls_connection *tls);

/* Kurzbeschreibung der ausgehandelten Verbindung, etwa
 * "TLS 1.3, X25519, AES-128-GCM". */
const char *tls_description(const struct tls_connection *tls);

#endif /* TLS_H */
