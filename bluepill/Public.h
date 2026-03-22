/*++

Module Name:

    public.h

Abstract:

    This module contains the common declarations shared by driver
    and user applications.

Environment:

    user and kernel

--*/

//
// Define an Interface Guid so that apps can find the device and talk to it.
//

DEFINE_GUID (GUID_DEVINTERFACE_bluepill,
    0x50b0841d,0xb755,0x44cd,0xbd,0xff,0xb9,0x87,0x28,0x80,0x17,0x81);
// {50b0841d-b755-44cd-bdff-b98728801781}
