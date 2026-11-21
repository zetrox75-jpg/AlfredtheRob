/* aio_resources.h - embedded payload IDs for the all-in-one launcher.
 *
 * Kept in one header so aio_launcher.c and aio_launcher.rc agree. The numbers
 * are arbitrary but must stay stable: the launcher extracts by ID.
 */
#ifndef AIO_RESOURCES_H
#define AIO_RESOURCES_H

#define AIO_RCDATA_KD2      101   /* kd2.exe            - the mapper            */
#define AIO_RCDATA_DRIVER   102   /* kernel-mode.sys    - WmPort image        */
#define AIO_RCDATA_OVERLAY  103   /* RobRunPort.exe     - the overlay          */
#define AIO_RCDATA_WEBVIEW2 104   /* WebView2Loader.dll - required beside exe  */

#endif
