#include "ldap-xplat.h"

#include "utils.h"

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__)

int
decrypt_response(CtxtHandle *handle, char *inToken, int inLen, char **outToken, int *outLen) {
	SecBufferDesc buff_desc;
	SecBuffer bufs[2];
	unsigned long qop = 0;
	int res;

	buff_desc.ulVersion = SECBUFFER_VERSION;
	buff_desc.cBuffers = 2;
	buff_desc.pBuffers = bufs;

	/* This buffer is for SSPI. */
	bufs[0].BufferType = SECBUFFER_STREAM;
	bufs[0].pvBuffer = inToken;
	bufs[0].cbBuffer = inLen;

	/* This buffer holds the application data. */
	bufs[1].BufferType = SECBUFFER_DATA;
	bufs[1].cbBuffer = 0;
	bufs[1].pvBuffer = NULL;

	res = DecryptMessage(handle, &buff_desc, 0, &qop);

	if (res == SEC_E_OK) {
		int maxlen = bufs[1].cbBuffer;
		char *p = (char *)malloc(maxlen);
		*outToken = p;
		*outLen = maxlen;
		memcpy(p, bufs[1].pvBuffer, bufs[1].cbBuffer);
	}

	return res;
}

int
encrypt_reply(CtxtHandle *handle, char *inToken, int inLen, char **outToken, int *outLen) {
	SecBufferDesc buff_desc;
	SecBuffer bufs[3];
	SecPkgContext_Sizes sizes;
	int res;

	res = QueryContextAttributes(handle, SECPKG_ATTR_SIZES, &sizes);

	buff_desc.ulVersion = SECBUFFER_VERSION;
	buff_desc.cBuffers = 3;
	buff_desc.pBuffers = bufs;

	/* This buffer is for SSPI. */
	bufs[0].BufferType = SECBUFFER_TOKEN;
	bufs[0].pvBuffer = malloc(sizes.cbSecurityTrailer);
	bufs[0].cbBuffer = sizes.cbSecurityTrailer;

	/* This buffer holds the application data. */
	bufs[1].BufferType = SECBUFFER_DATA;
	bufs[1].cbBuffer = inLen;
	bufs[1].pvBuffer = malloc(inLen);

	memcpy(bufs[1].pvBuffer, inToken, inLen);

	/* This buffer is for SSPI. */
	bufs[2].BufferType = SECBUFFER_PADDING;
	bufs[2].cbBuffer = sizes.cbBlockSize;
	bufs[2].pvBuffer = malloc(sizes.cbBlockSize);

	res = EncryptMessage(handle, SECQOP_WRAP_NO_ENCRYPT, &buff_desc, 0);

	if (res == SEC_E_OK) {
		int maxlen = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
		char *p = (char *)malloc(maxlen);
		*outToken = p;
		*outLen = maxlen;
		memcpy(p, bufs[0].pvBuffer, bufs[0].cbBuffer);
		p += bufs[0].cbBuffer;
		memcpy(p, bufs[1].pvBuffer, bufs[1].cbBuffer);
		p += bufs[1].cbBuffer;
		memcpy(p, bufs[2].pvBuffer, bufs[2].cbBuffer);
	}

	return res;
}

/* It does what it says: no verification on the server cert. */
BOOLEAN _cdecl noverify(PLDAP Connection, PCCERT_CONTEXT *ppServerCert) {
	return 1;
}

int LDAP_initialization(LDAP **ld, PyObject *url, int tls_option) {
	int rc;
	int portnum;
	char *hoststr = NULL;
	const int version = LDAP_VERSION3;
	const int tls_settings = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_SERVERNAME_CHECK;
	PyObject *scheme = PyObject_GetAttrString(url, "scheme");
	PyObject *host = PyObject_GetAttrString(url, "host");
	PyObject *port = PyObject_GetAttrString(url, "port");

	if (scheme == NULL || host == NULL || port == NULL) return -1;

	hoststr = PyObject2char(host);
	portnum = PyLong_AsLong(port);
	Py_DECREF(host);
	Py_DECREF(port);

	if (hoststr == NULL) return -1;

	if (PyUnicode_CompareWithASCIIString(scheme, "ldaps") == 0) {
		*ld = ldap_sslinit(hoststr, portnum, 1);
	} else {
		*ld = ldap_init(hoststr, portnum);
	}
	Py_DECREF(scheme);
	if (ld == NULL) return -1;
	ldap_set_option(*ld, LDAP_OPT_PROTOCOL_VERSION, &version);
	switch (tls_option) {
		case -1:
			/* Cert policy is not set, nothing to do.*/
			break;
		case 2:
		case 4:
			/* Cert policy is demand or try, then standard procedure. */
			break;
		case 0:
		case 3:
			/* Cert policy is never or allow, then set TLS settings. */
			ldap_set_option(*ld, 0x43, &tls_settings);
			ldap_set_option(*ld, LDAP_OPT_SERVER_CERTIFICATE, &noverify);
			break;
	}
	rc = ldap_connect(*ld, NULL);
	return rc;
}

int LDAP_bind(LDAP *ld, ldapConnectionInfo *info, LDAPMessage *result, int *msgid) {
	int rc;
	int len = 0;
	int gssapi_decrpyt = 0;
	unsigned long contextattr;
	struct berval cred;
	char *output = NULL;
	SecBufferDesc out_buff_desc;
	SecBuffer out_buff;
	SecBufferDesc in_buff_desc;
	SecBuffer in_buff;
	struct berval *response = NULL;

	if (result != NULL) {
		rc = ldap_parse_extended_result(ld, result, NULL, &response, 1);
		if (rc != LDAP_SUCCESS) return rc;
	}

	if (strcmp(info->mech, "SIMPLE") != 0) {
		/* Use SASL bind. */

		if (response == NULL) {
			/* First function call, no server response. */
			out_buff_desc.ulVersion = 0;
			out_buff_desc.cBuffers = 1;
			out_buff_desc.pBuffers = &out_buff;

			out_buff.BufferType = SECBUFFER_TOKEN;
			out_buff.pvBuffer = NULL;

			rc = InitializeSecurityContext(info->credhandle, NULL, info->targetName, ISC_REQ_MUTUAL_AUTH | ISC_REQ_ALLOCATE_MEMORY,
				0, 0, NULL, 0, info->ctxhandle, &out_buff_desc, &contextattr, NULL);
		} else {
			in_buff_desc.ulVersion = SECBUFFER_VERSION;
			in_buff_desc.cBuffers = 1;
			in_buff_desc.pBuffers = &in_buff;

			in_buff.cbBuffer = response->bv_len;
			in_buff.BufferType = SECBUFFER_TOKEN;
			in_buff.pvBuffer = response->bv_val;
			if (gssapi_decrpyt) {
				char *input = NULL;
				rc = decrypt_response(info->ctxhandle, response->bv_val, response->bv_len, &output, &len);
				input = output;
				rc = encrypt_reply(info->ctxhandle, input, len, &output, &len);
			} else {
				rc = InitializeSecurityContext(info->credhandle, info->ctxhandle, info->targetName, ISC_REQ_MUTUAL_AUTH |
					ISC_REQ_ALLOCATE_MEMORY, 0, 0, &in_buff_desc, 0, info->ctxhandle, &out_buff_desc, &contextattr, NULL);
			}
		}

		switch (rc) {
		case SEC_I_COMPLETE_NEEDED:
		case SEC_I_COMPLETE_AND_CONTINUE:
			CompleteAuthToken(info->ctxhandle, &out_buff_desc);
			break;
		case SEC_E_OK:
			if (strcmp(info->mech, "GSSAPI") == 0) {
				gssapi_decrpyt = 1;
			}
			break;
		case SEC_I_CONTINUE_NEEDED:
			break;
		case SEC_E_INVALID_HANDLE:
		case SEC_E_INVALID_TOKEN:
			//TODO: better error;
			PyErr_BadInternalCall();
			return -1;
		default:
			break;
		}

		cred.bv_len = out_buff.cbBuffer;
		cred.bv_val = (char *)out_buff.pvBuffer;

		if (gssapi_decrpyt) {
			cred.bv_len = len;
			cred.bv_val = output;
		}
		/* Empty binddn is needed to change "" to avoid param error. */
		if (info->binddn == NULL) info->binddn = "";
		rc = ldap_sasl_bind(ld, info->binddn, info->mech, &cred, NULL, NULL, msgid);
		/* Get the last error code form the LDAP struct. */
		ldap_get_option(ld, LDAP_OPT_ERROR_NUMBER, &rc);
	} else {
		/* Use simple bind with bind DN and password. */
		rc = ldap_simple_bind(ld, info->binddn, (char *)(info->creds->Password));
		*msgid = rc;
		if (rc > 0) rc = 0;
	}
	return rc;
}

int LDAP_unbind(LDAP *ld) {
	return ldap_unbind(ld);
}

int LDAP_abandon(LDAP *ld, int msgid) {
	return ldap_abandon(ld, msgid);
}

int
ldap_start_tls(LDAP *ld, LDAPControl **serverctrls, LDAPControl **clientctrls, int *msgidp) {
	return ldap_extended_operation(ld, LDAP_START_TLS_OID, NULL, serverctrls, clientctrls, msgidp);
}

/* Create a struct with the necessary infos and structs for binding an
   LDAP Server.
*/
void *
create_conn_info(LDAP *ld, char *mech, PyObject *creds) {
	int rc = -1;
	ldapConnectionInfo *defaults = NULL;
	PyObject *tmp = NULL;
	char *secpack = NULL;
	char *authcid = NULL;
	char *authzid = NULL;
	char *binddn = NULL;
	char *passwd = NULL;
	char *realm = NULL;
	SEC_WINNT_AUTH_IDENTITY *wincreds = NULL;

	wincreds = (SEC_WINNT_AUTH_IDENTITY *)malloc(sizeof(SEC_WINNT_AUTH_IDENTITY));
	if (wincreds == NULL) return (void *)PyErr_NoMemory();
	memset(wincreds, 0, sizeof(wincreds));

	defaults = (ldapConnectionInfo *)malloc(sizeof(ldapConnectionInfo));
	if (defaults == NULL) {
		free(wincreds);
		return (void *)PyErr_NoMemory();
	}

	defaults->credhandle = (CredHandle *)malloc(sizeof(CredHandle));
	if (defaults->credhandle == NULL) {
		free(wincreds);
		free(defaults);
		return (void *)PyErr_NoMemory();
	}

	/* Copy hostname from LDAP struct to create a valid targetName(SPN). */
	defaults->targetName = (char *)malloc(strlen(ld->ld_host) + 6);
	if (defaults->targetName == NULL) {
		free(wincreds);
		free(defaults->credhandle);
		free(defaults);
		return (void *)PyErr_NoMemory();
	}
	sprintf_s(defaults->targetName, strlen(ld->ld_host) + 6, "ldap/%hs", ld->ld_host);

	defaults->mech = mech;

	/* Get credential information, if it's given. */
	if (PyTuple_Check(creds) && PyTuple_Size(creds) > 1) {
		if (strcmp(mech, "SIMPLE") == 0) {
			tmp = PyTuple_GetItem(creds, 0);
			binddn = PyObject2char(tmp);
		}
		else {
			tmp = PyTuple_GetItem(creds, 0);
			authcid = PyObject2char(tmp);
			tmp = PyTuple_GetItem(creds, 2);
			realm = PyObject2char(tmp);
		}
		tmp = PyTuple_GetItem(creds, 1);
		passwd = PyObject2char(tmp);
	}

	wincreds->User = (unsigned char *)authcid;
	if (authcid != NULL) wincreds->UserLength = (unsigned long)strlen(authcid);
	else wincreds->UserLength = 0;
	wincreds->Password = (unsigned char *)passwd;
	if (passwd != NULL) wincreds->PasswordLength = (unsigned long)strlen(passwd);
	else wincreds->PasswordLength = 0;
	wincreds->Domain = (unsigned char *)realm;
	if (realm != NULL) wincreds->DomainLength = (unsigned long)strlen(realm);
	else wincreds->DomainLength = 0;

	wincreds->Flags = SEC_WINNT_AUTH_IDENTITY_UNICODE;

	defaults->authzid = authzid;
	defaults->binddn = binddn;
	defaults->creds = wincreds;
	defaults->ctxhandle = NULL;

	if (strcmp(mech, "SIMPLE") != 0) {
		/* Select corresponding security packagename from the mechanism name. */
		if (strcmp(mech, "DIGEST-MD5") == 0) {
			secpack = "WDigest";
		} else if (strcmp(mech, "GSSAPI") == 0) {
			secpack = "Kerberos";
		}

		/* Create credential handler. */
		rc = AcquireCredentialsHandle(NULL, secpack, SECPKG_CRED_OUTBOUND, NULL, wincreds, NULL, NULL, defaults->credhandle, NULL);
		if (rc != SEC_E_OK) {
			PyErr_BadInternalCall();
			return NULL;
		}
	}
	return defaults;
}

/* Dealloc an ldapConnectionInfo struct. */
void
dealloc_conn_info(ldapConnectionInfo* info) {
	if (info->authzid) free(info->authzid);
	if (info->binddn && strcmp(info->binddn, "") != 0) free(info->binddn);
	if (info->mech) free(info->mech);
	if (info->targetName) free(info->targetName);
	if (info->credhandle) free(info->credhandle);
	if (info->creds->Domain) free(info->creds->Domain);
	if (info->creds->Password) free(info->creds->Password);
	if (info->creds->User) free(info->creds->User);
	free(info->creds);
	free(info);
}

#else

int LDAP_initialization(LDAP **ld, PyObject *url, int tls_option) {
	int rc;
	char *addrstr;
	const int version = LDAP_VERSION3;

	PyObject *addr = PyObject_CallMethod(url, "get_address", NULL);
	if (addr == NULL) return -1;
	addrstr = PyObject2char(addr);
	Py_DECREF(addr);
	if (addrstr == NULL) return -1;

	rc = ldap_initialize(ld, addrstr);
	if (rc != LDAP_SUCCESS) return rc;

	ldap_set_option(*ld, LDAP_OPT_PROTOCOL_VERSION, &version);
	if (tls_option != -1) {
		ldap_set_option(*ld, LDAP_OPT_X_TLS_REQUIRE_CERT, &tls_option);
		/* Set TLS option globally. */
		ldap_set_option(NULL, LDAP_OPT_X_TLS_REQUIRE_CERT, &tls_option);
	}
	return rc;
}

int LDAP_bind(LDAP *ld, ldapConnectionInfo *info, LDAPMessage *result, int *msgid) {
	int rc;
	LDAPControl	**sctrlsp = NULL;
	struct berval passwd;

	/* Mechanism is set, use SASL interactive bind. */
	if (strcmp(info->mech, "SIMPLE") != 0) {
		if (info->passwd == NULL) info->passwd = "";
		rc = ldap_sasl_interactive_bind(ld, info->binddn, info->mech, sctrlsp, NULL, LDAP_SASL_QUIET, sasl_interact, info, result, &(info->rmech), msgid);
	} else {
		if (info->passwd  == NULL) {
			passwd.bv_len = 0;
		} else {
			passwd.bv_len = strlen(info->passwd );
		}
		passwd.bv_val = info->passwd ;
		rc = ldap_sasl_bind(ld, info->binddn, LDAP_SASL_SIMPLE, &passwd, sctrlsp, NULL, msgid);
	}

	ldap_msgfree(result);

	return rc;
}

int LDAP_unbind(LDAP *ld) {
	return ldap_unbind_ext((ld), NULL, NULL);
}

int LDAP_abandon(LDAP *ld, int msgid ) {
	return ldap_abandon_ext(ld, msgid, NULL, NULL);
}

/*  This function is based on the OpenLDAP liblutil's sasl.c source
    file for creating a lutilSASLdefaults struct with default values based on
    the given parameters or client's options. */
void *
create_conn_info(LDAP *ld, char *mech, PyObject *creds) {
	ldapConnectionInfo *defaults = NULL;
	PyObject *tmp = NULL;
	char *authcid = NULL;
	char *authzid = NULL;
	char *binddn = NULL;
	char *passwd = NULL;
	char *realm = NULL;

	/* Get credential information, if it's given. */
	if (PyTuple_Check(creds) && PyTuple_Size(creds) > 1) {
		if (strcmp(mech, "SIMPLE") == 0) {
			tmp = PyTuple_GetItem(creds, 0);
			binddn = PyObject2char(tmp);
		} else {
			tmp = PyTuple_GetItem(creds, 0);
			authcid = PyObject2char(tmp);
			tmp = PyTuple_GetItem(creds, 2);
			realm = PyObject2char(tmp);
		}
		tmp = PyTuple_GetItem(creds, 1);
		passwd = PyObject2char(tmp);
	}

	defaults = ber_memalloc(sizeof(ldapConnectionInfo));
	if(defaults == NULL) return (void *)PyErr_NoMemory();

	defaults->mech = mech ? ber_strdup(mech) : NULL;
	defaults->realm = realm ? ber_strdup(realm) : NULL;
	defaults->authcid = authcid ? ber_strdup(authcid) : NULL;
	defaults->passwd = passwd ? ber_strdup(passwd) : NULL;
	defaults->authzid = authzid ? ber_strdup(authzid) : NULL;

	if (defaults->mech == NULL) {
		ldap_get_option(ld, LDAP_OPT_X_SASL_MECH, &defaults->mech);
	}
	if (defaults->realm == NULL) {
		ldap_get_option(ld, LDAP_OPT_X_SASL_REALM, &defaults->realm);
	}
	if (defaults->authcid == NULL) {
		ldap_get_option(ld, LDAP_OPT_X_SASL_AUTHCID, &defaults->authcid);
	}
	if (defaults->authzid == NULL) {
		ldap_get_option(ld, LDAP_OPT_X_SASL_AUTHZID, &defaults->authzid);
	}
	defaults->resps = NULL;
	defaults->nresps = 0;
	defaults->binddn = binddn;
	defaults->rmech = NULL;

	return defaults;
}

/* Dealloc an ldapConnectionInfo struct. */
void
dealloc_conn_info(ldapConnectionInfo* info) {
	if (info->authcid) free(info->authcid);
	if (info->authzid) free(info->authzid);
	if (info->binddn) free(info->binddn);
	if (info->mech) free(info->mech);
	if (info->passwd) free(info->passwd);
	if (info->realm) free(info->realm);
	free(info);
}

/*	This function is based on the lutil_sasl_interact() function, which can
    be found in the OpenLDAP liblutil's sasl.c source. I did some simplification
    after some google and stackoverflow reasearches, and hoping to not cause
    any problems. */
int
sasl_interact(LDAP *ld, unsigned flags, void *defs, void *in) {
    sasl_interact_t *interact = (sasl_interact_t*)in;
    const char *dflt = interact->defresult;
    ldapConnectionInfo *defaults = (ldapConnectionInfo *)defs;

	while (interact->id != SASL_CB_LIST_END) {
		switch(interact->id) {
			case SASL_CB_GETREALM:
				if (defaults) dflt = defaults->realm;
				break;
			case SASL_CB_AUTHNAME:
				if (defaults) dflt = defaults->authcid;
				break;
			case SASL_CB_PASS:
				if (defaults) dflt = defaults->passwd;
				break;
			case SASL_CB_USER:
				if (defaults) dflt = defaults->authzid;
				break;
			case SASL_CB_NOECHOPROMPT:
				break;
			case SASL_CB_ECHOPROMPT:
				break;
		}

		interact->result = (dflt && *dflt) ? dflt : (char*)"";
		interact->len = strlen( (char*)interact->result );

		interact++;
	}
	return LDAP_SUCCESS;
}
#endif