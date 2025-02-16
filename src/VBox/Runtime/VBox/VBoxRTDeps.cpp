/* $Id$ */
/** @file
 * IPRT - VBoxRT.dll/so dependencies.
 */

/*
 * Copyright (C) 2006-2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#ifndef RT_NO_GIP
# include <VBox/sup.h>
#endif
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/buildconfig.h>
#include <iprt/system.h>

#include <libxml/catalog.h>
#include <libxml/globals.h>
#include <openssl/md5.h>
#include <openssl/rc4.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
PFNRT g_VBoxRTDeps[] =
{
#ifndef RT_NO_GIP
    (PFNRT)SUPR3Init,
    (PFNRT)SUPR3PageAllocEx,
    (PFNRT)SUPR3LoadVMM,
    (PFNRT)SUPSemEventCreate,
    (PFNRT)SUPTracerFireProbe,
    (PFNRT)SUPGetTscDeltaSlow,
#endif
    (PFNRT)xmlLoadCatalogs,
    (PFNRT)MD5_Init,
    (PFNRT)RC4,
    (PFNRT)RC4_set_key,
    (PFNRT)PEM_read_bio_X509,
    (PFNRT)PEM_read_bio_PrivateKey,
    (PFNRT)X509_free,
    (PFNRT)X509_verify_cert_error_string,
    (PFNRT)i2d_X509,
    (PFNRT)i2d_X509,
    (PFNRT)i2d_PublicKey,
    (PFNRT)RSA_generate_key,
    (PFNRT)RSA_generate_key_ex,
    (PFNRT)DH_generate_parameters,
    (PFNRT)DH_generate_parameters_ex,
    (PFNRT)RAND_load_file,
    (PFNRT)CRYPTO_set_dynlock_create_callback,
    (PFNRT)CRYPTO_set_dynlock_lock_callback,
    (PFNRT)CRYPTO_set_dynlock_destroy_callback,
    (PFNRT)RTAssertShouldPanic,
    (PFNRT)ASMAtomicReadU64,
    (PFNRT)ASMAtomicCmpXchgU64,
    (PFNRT)RTBldCfgRevision,
    (PFNRT)SSL_free,
    (PFNRT)SSL_library_init,
    (PFNRT)SSL_load_error_strings,
    (PFNRT)SSL_CTX_free,
    (PFNRT)SSL_CTX_use_certificate_file,
    (PFNRT)SSLv23_method,
    (PFNRT)TLSv1_server_method,
    NULL
};

