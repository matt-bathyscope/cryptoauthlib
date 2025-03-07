/**
 * \file
 * \brief CryptoAuthLib Basic API methods for SecureBoot command.
 *
 * The SecureBoot command provides support for secure boot of an external MCU
 * or MPU.
 *
 * \note List of devices that support this command - ATECC608A/B. Refer to
 *       device datasheet for full details.
 *
 * \copyright (c) 2015-2020 Microchip Technology Inc. and its subsidiaries.
 *
 * \page License
 *
 * Subject to your compliance with these terms, you may use Microchip software
 * and any derivatives exclusively with Microchip products. It is your
 * responsibility to comply with third party license terms applicable to your
 * use of third party software (including open source software) that may
 * accompany Microchip software.
 *
 * THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
 * EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
 * WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
 * PARTICULAR PURPOSE. IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT,
 * SPECIAL, PUNITIVE, INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE
 * OF ANY KIND WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF
 * MICROCHIP HAS BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE
 * FORESEEABLE. TO THE FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL
 * LIABILITY ON ALL CLAIMS IN ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED
 * THE AMOUNT OF FEES, IF ANY, THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR
 * THIS SOFTWARE.
 */

#include "cryptoauthlib.h"

#if CALIB_SECUREBOOT_MAC_EN
#include "host/atca_host.h"
#endif

#if CALIB_SECUREBOOT_EN
/** \brief Executes Secure Boot command, which provides support for secure
 *          boot of an external MCU or MPU.
 *
 * \param[in]  device     Device context pointer
 * \param[in]  mode       Mode determines what operations the SecureBoot
 *                        command performs.
 * \param[in]  param2     Not used, must be 0.
 * \param[in]  digest     Digest of the code to be verified (32 bytes).
 * \param[in]  signature  Signature of the code to be verified (64 bytes). Can
 *                        be NULL when using the FullStore mode.
 * \param[out] mac        Validating MAC will be returned here (32 bytes). Can
 *                        be NULL if not required.
 *
 * \return ATCA_SUCCESS on success, otherwise an error code.
 */
ATCA_STATUS calib_secureboot(ATCADevice device, uint8_t mode, uint16_t param2, const uint8_t* digest, const uint8_t* signature, uint8_t* mac)
{
    ATCAPacket * packet = NULL;
    ATCA_STATUS status;

    do
    {
        if ((device == NULL) || (digest == NULL))
        {
            status = ATCA_TRACE(ATCA_BAD_PARAM, "NULL pointer received");
            break;
        }

        #if (CA_MAX_PACKET_SIZE < (ATCA_CMD_SIZE_MIN + SECUREBOOT_DIGEST_SIZE + SECUREBOOT_SIGNATURE_SIZE))
        #if ATCA_PREPROCESSOR_WARNING
        #warning "CA_MAX_PACKET_SIZE will not support full or fullcopy mode in secureboot command"
        #endif
        if (NULL != signature)
        {
            status = ATCA_TRACE(ATCA_INVALID_SIZE, "Unsupported parameter");
        }
        #endif

        packet = calib_packet_alloc();
        if(NULL == packet)
        {
            (void)ATCA_TRACE(ATCA_ALLOC_FAILURE, "calib_packet_alloc - failed");
            status = ATCA_ALLOC_FAILURE;
            break;
        }
        
        (void)memset(packet, 0x00, sizeof(ATCAPacket));

        packet->param1 = mode;
        packet->param2 = param2;

        (void)memcpy(packet->data, digest, SECUREBOOT_DIGEST_SIZE);

        if (NULL != signature)
        {
            (void)memcpy(&packet->data[SECUREBOOT_DIGEST_SIZE], signature, SECUREBOOT_SIGNATURE_SIZE);
        }

        if ((status = atSecureBoot(atcab_get_device_type_ext(device), packet)) != ATCA_SUCCESS)
        {
            (void)ATCA_TRACE(status, "atSecureBoot - failed");
            break;
        }

        if ((status = atca_execute_command(packet, device)) != ATCA_SUCCESS)
        {
            (void)ATCA_TRACE(status, "calib_secureboot - execution failed");
            break;
        }

        if ((mac != NULL) && (packet->data[ATCA_COUNT_IDX] >= SECUREBOOT_RSP_SIZE_MAC))
        {
            (void)memcpy(mac, &packet->data[ATCA_RSP_DATA_IDX], SECUREBOOT_MAC_SIZE);
        }

    } while (false);

    calib_packet_free(packet);
    return status;
}
#endif /* CALIB_SECUREBOOT_EN */

#if CALIB_SECUREBOOT_MAC_EN
/** \brief Executes Secure Boot command with encrypted digest and validated
 *          MAC response using the IO protection key.
 *
 * \param[in]  device       Device context pointer
 * \param[in]  mode         Mode determines what operations the SecureBoot
 *                          command performs.
 * \param[in]  digest       Digest of the code to be verified (32 bytes).
 *                          This is the plaintext digest (not encrypted).
 * \param[in]  signature    Signature of the code to be verified (64 bytes). Can
 *                          be NULL when using the FullStore mode.
 * \param[in]  num_in       Host nonce (20 bytes).
 * \param[in]  io_key       IO protection key (32 bytes).
 * \param[out] is_verified  Verify result is returned here.
 *
 * \return ATCA_SUCCESS on success, otherwise an error code.
 */
ATCA_STATUS calib_secureboot_mac(ATCADevice device, uint8_t mode, const uint8_t* digest, const uint8_t* signature, const uint8_t* num_in, const uint8_t* io_key,
                                 bool* is_verified)
{
    ATCA_STATUS status;
    atca_temp_key_t tempkey;
    atca_nonce_in_out_t nonce_params;
    atca_secureboot_enc_in_out_t sboot_enc_params;
    atca_secureboot_mac_in_out_t sboot_mac_params;
    uint8_t rand_out[RANDOM_NUM_SIZE];
    uint8_t key[ATCA_KEY_SIZE];
    uint8_t digest_enc[SECUREBOOT_DIGEST_SIZE];
    uint8_t mac[SECUREBOOT_MAC_SIZE];
    uint8_t host_mac[SECUREBOOT_MAC_SIZE];
    uint8_t buf[2];

    do
    {
        if ((is_verified == NULL) || (digest == NULL) || (num_in == NULL) || (io_key == NULL))
        {
            status = ATCA_TRACE(ATCA_BAD_PARAM, "NULL pointer received");
            break;
        }

        *is_verified = false;

        // Setup Nonce command to create nonce combining host (num_in) and
        // device (RNG) nonces
        (void)memset(&tempkey, 0, sizeof(tempkey));
        (void)memset(&nonce_params, 0, sizeof(nonce_params));
        nonce_params.mode = NONCE_MODE_SEED_UPDATE;
        nonce_params.zero = 0;
        nonce_params.num_in = num_in;
        nonce_params.rand_out = rand_out;
        nonce_params.temp_key = &tempkey;

        // Initialize TempKey with nonce
        if (ATCA_SUCCESS != (status = calib_nonce_base(device, nonce_params.mode, nonce_params.zero, nonce_params.num_in, rand_out)))
        {
            (void)ATCA_TRACE(status, "calib_nonce_base - failed");
            break;
        }

        // Calculate nonce (TempKey) value
        if (ATCA_SUCCESS != (status = atcah_nonce(&nonce_params)))
        {
            (void)ATCA_TRACE(status, "atcah_nonce - failed");
            break;
        }

        // Encrypt the digest
        (void)memset(&sboot_enc_params, 0, sizeof(sboot_enc_params));
        sboot_enc_params.digest = digest;
        sboot_enc_params.io_key = io_key;
        sboot_enc_params.temp_key = &tempkey;
        sboot_enc_params.hashed_key = key;
        sboot_enc_params.digest_enc = digest_enc;
        if (ATCA_SUCCESS != (status = atcah_secureboot_enc(&sboot_enc_params)))
        {
            (void)ATCA_TRACE(status, "atcah_secureboot_enc - failed");
            break;
        }

        // Prepare MAC calculator
        (void)memset(&sboot_mac_params, 0, sizeof(sboot_mac_params));
        sboot_mac_params.mode = mode | SECUREBOOT_MODE_ENC_MAC_FLAG;
        sboot_mac_params.param2 = 0;
        sboot_mac_params.hashed_key = sboot_enc_params.hashed_key;
        sboot_mac_params.digest = digest;
        sboot_mac_params.signature = signature;
        sboot_mac_params.mac = host_mac;

        // Run the SecureBoot command
        if (ATCA_SUCCESS != (status = calib_secureboot(device, sboot_mac_params.mode, sboot_mac_params.param2, digest_enc, signature, mac)))
        {
            // Verify failed...
            if (ATCA_CHECKMAC_VERIFY_FAILED == status)
            {
                // Still consider this a command success
                *is_verified = false;
                status = ATCA_SUCCESS;
            }
            break;
        }

        // Read the SecureBootConfig field out of the configuration zone, which
        // is required to properly calculate the expected MAC
        if (ATCA_SUCCESS != (status = calib_read_bytes_zone(device, ATCA_ZONE_CONFIG, 0, SECUREBOOTCONFIG_OFFSET, buf, 2)))
        {
            (void)ATCA_TRACE(status, "calib_read_bytes_zone - failed");
            break;
        }
        sboot_mac_params.secure_boot_config = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);

        // Calculate the expected MAC
        if (ATCA_SUCCESS != (status = atcah_secureboot_mac(&sboot_mac_params)))
        {
            (void)ATCA_TRACE(status, "atcah_secureboot_mac - failed");
            break;
        }

        *is_verified = (memcmp(host_mac, mac, SECUREBOOT_MAC_SIZE) == 0);
    } while (false);

    return status;
}
#endif /* CALIB_SECUREBOOT_MAC_EN */
