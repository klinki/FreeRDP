/** Tests key logging for independent TLS sessions sharing one diagnostic file. */
#include <stdio.h>
#include <string.h>

#include <winpr/crt.h>
#include <winpr/file.h>
#include <winpr/path.h>
#include <winpr/thread.h>
#include <winpr/sysinfo.h>
#include <openssl/rsa.h>

#include "../tls.h"

static BOOL handshake_step(SSL* ssl)
{
	if (SSL_is_init_finished(ssl))
		return TRUE;
	const int result = SSL_do_handshake(ssl);
	if (result == 1)
		return TRUE;
	const int error = SSL_get_error(ssl, result);
	return (error == SSL_ERROR_WANT_READ) || (error == SSL_ERROR_WANT_WRITE);
}

/* Like the main transport and the UDP tunnel, each connection has its own SSL_CTX. */
static BOOL run_session(SSL_CTX* serverContext, const char* file, char randomHex[65])
{
	BOOL success = FALSE;
	BYTE random[32] = WINPR_C_ARRAY_INIT;
	SSL_CTX* clientContext = SSL_CTX_new(TLS_client_method());
	SSL* client = clientContext ? SSL_new(clientContext) : nullptr;
	SSL* server = SSL_new(serverContext);
	BIO* clientBio = nullptr;
	BIO* serverBio = nullptr;
	if (!client || !server || !freerdp_tls_set_secrets_file(client, file))
		goto out;
	if (BIO_new_bio_pair(&clientBio, 0, &serverBio, 0) != 1)
		goto out;
	SSL_set_bio(client, clientBio, clientBio);
	SSL_set_bio(server, serverBio, serverBio);
	SSL_set_connect_state(client);
	SSL_set_accept_state(server);
	for (size_t x = 0; x < 100; x++)
	{
		if (!handshake_step(client) || !handshake_step(server))
			goto out;
		if (SSL_is_init_finished(client) && SSL_is_init_finished(server))
		{
			if (SSL_get_client_random(client, random, sizeof(random)) != sizeof(random))
				goto out;
			for (size_t i = 0; i < sizeof(random); i++)
				(void)snprintf(&randomHex[2 * i], 3, "%02x", random[i]);
			success = TRUE;
			break;
		}
	}
out:
	SSL_free(client);
	SSL_free(server);
	SSL_CTX_free(clientContext);
	return success;
}

int TestTlsSecretsFile(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	int result = -1;
	char* file = nullptr;
	HANDLE tempFile = INVALID_HANDLE_VALUE;
	char randoms[3][65] = WINPR_C_ARRAY_INIT;
	BOOL found[2][2] = WINPR_C_ARRAY_INIT;
	FILE* stream = nullptr;
	EVP_PKEY* key = nullptr;
	EVP_PKEY_CTX* keyContext = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
	X509* certificate = X509_new();
	SSL_CTX* serverContext = SSL_CTX_new(TLS_server_method());
	if (!keyContext || !certificate || !serverContext)
		goto out;
	if ((EVP_PKEY_keygen_init(keyContext) <= 0) ||
	    (EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext, 2048) <= 0) ||
	    (EVP_PKEY_keygen(keyContext, &key) <= 0))
		goto out;
	if ((X509_set_version(certificate, 2) != 1) ||
	    (ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) != 1) ||
	    !X509_gmtime_adj(X509_getm_notBefore(certificate), 0) ||
	    !X509_gmtime_adj(X509_getm_notAfter(certificate), 3600) ||
	    (X509_set_pubkey(certificate, key) != 1))
		goto out;
	X509_NAME* name = X509_get_subject_name(certificate);
	if ((X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
	                               (const unsigned char*)"localhost", -1, -1, 0) != 1) ||
	    (X509_set_issuer_name(certificate, name) != 1) ||
	    (X509_sign(certificate, key, EVP_sha256()) <= 0) ||
	    (SSL_CTX_use_certificate(serverContext, certificate) != 1) ||
	    (SSL_CTX_use_PrivateKey(serverContext, key) != 1) ||
	    (SSL_CTX_set_min_proto_version(serverContext, TLS1_3_VERSION) != 1))
		goto out;
	file = GetKnownSubPathV(KNOWN_PATH_TEMP, "TestTlsSecretsFile-%" PRIu32 "-%" PRIu64 ".txt",
	                        GetCurrentProcessId(), GetTickCount64());
	if (!file)
		goto out;
	tempFile = CreateFileA(file, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
	                       FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (tempFile == INVALID_HANDLE_VALUE)
	{
		free(file);
		file = nullptr;
		goto out;
	}
	(void)CloseHandle(tempFile);
	if (!run_session(serverContext, file, randoms[0]) ||
	    !run_session(serverContext, file, randoms[1]) ||
	    !run_session(serverContext, nullptr, randoms[2]))
		goto out;
	stream = winpr_fopen(file, "r");
	if (!stream)
		goto out;
	char line[1024] = WINPR_C_ARRAY_INIT;
	while (fgets(line, sizeof(line), stream))
	{
		char label[64] = WINPR_C_ARRAY_INIT;
		char randomHex[65] = WINPR_C_ARRAY_INIT;
		if (sscanf(line, "%63s %64s", label, randomHex) != 2)
			goto out;
		/* The session without an explicit file must not leak keys to another session's file. */
		if (strcmp(randomHex, randoms[2]) == 0)
			goto out;
		for (size_t i = 0; i < 2; i++)
		{
			if (strcmp(randomHex, randoms[i]) != 0)
				continue;
			if (strcmp(label, "CLIENT_TRAFFIC_SECRET_0") == 0)
				found[i][0] = TRUE;
			if (strcmp(label, "SERVER_TRAFFIC_SECRET_0") == 0)
				found[i][1] = TRUE;
		}
	}
	if (found[0][0] && found[0][1] && found[1][0] && found[1][1])
		result = 0;
out:
	if (result != 0)
		(void)fprintf(stderr, "TLS sessions did not produce the expected opt-in key logs\n");
	if (stream)
		(void)fclose(stream);
	if (file)
		(void)winpr_DeleteFile(file);
	free(file);
	SSL_CTX_free(serverContext);
	X509_free(certificate);
	EVP_PKEY_free(key);
	EVP_PKEY_CTX_free(keyContext);
	return result;
}
