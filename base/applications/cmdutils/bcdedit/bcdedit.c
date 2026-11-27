/*
 * PROJECT:     LiberNT Boot Configuration Data editor
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     bcdedit.exe - lists and edits the boot configuration data store.
 *              The store is the system BCD hive (HKLM\BCD00000000) by default, or a hive
 *              file opened with /store. Object and element identifiers and the element/device
 *              layout come from the boot loader's own definitions (reactos/bcddef.h and
 *              bootloader/include/bcd.h); the registry layout is Objects\{id}\Description\Type
 *              and Objects\{id}\Elements\<type>\Element.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <strsafe.h>

#include <reactos/bcddef.h>
#include "resource.h"

#define BCD_SYSTEM_STORE    L"BCD00000000"
#define BCD_FIELD_WIDTH     24
#define BCD_MAX_ELEMENTS    256

/* The fixed, store-independent object identifiers, so /enum shows {bootmgr} and friends. */
static const struct { PCWSTR Guid; PCWSTR Alias; } g_WellKnownIds[] =
{
    { L"{9dea862c-5cdd-4e70-acc1-f32b344d4795}", L"{bootmgr}" },
    { L"{a5a30fa2-3d06-4e9f-b5f4-a01df9d1fcba}", L"{fwbootmgr}" },
    { L"{b2721d73-1db4-4c62-bf78-c548a880142d}", L"{memdiag}" },
    { L"{466f5a88-0af2-4f76-9038-095b170dc21c}", L"{ntldr}" },
    { L"{ae5534e0-a924-466c-b836-758539a3ee3a}", L"{ramdiskoptions}" },
    { L"{4636856e-540f-4170-a130-a84776f4c654}", L"{dbgsettings}" },
    { L"{0ce4991b-e6b3-4b16-b23c-5e0d9250e5d9}", L"{emssettings}" },
    { L"{5189b25c-5558-4bf2-bca4-289b11bd29e2}", L"{badmemory}" },
    { L"{6efb52bf-1766-41db-a6b3-0ee5eff72bd7}", L"{bootloadersettings}" },
    { L"{7ff607e0-4395-11db-b0de-0800200c9a66}", L"{hypervisorsettings}" },
    { L"{1afa9c49-16ab-4a5c-901b-212802da9460}", L"{resumeloadersettings}" },
    { L"{7ea2e1ac-2e61-4728-aaa3-896d9d0a9f0e}", L"{globalsettings}" },
};

/* Element value formats and object classes come from the Windows 11 BCD element reference
 * (names and ids are interoperability identifiers). The scope disambiguates the handful of ids
 * and names that mean different things in different object classes, for example 0x21000001 is
 * osdevice on a loader but filedevice on a resume object. Unknown ids print as custom:<type>. */
typedef enum _BCD_SCOPE
{
    S_ANY = 0,      /* a library/global element, or an id used by only one class */
    S_BOOTMGR,
    S_OSLOADER,
    S_RESUME,
    S_MEMTEST,
    S_STARTUP,
    S_DEVICE,
    S_MOBILE,
    S_OTHER
} BCD_SCOPE;

static const struct { ULONG Type; BCD_SCOPE Scope; PCWSTR Name; } g_ElementNames[] =
{
    { 0x11000001, S_ANY, L"device" },
    { 0x11000043, S_ANY, L"bootstatdevice" },
    { 0x11000084, S_ANY, L"windowssyspart" },
    { 0x12000002, S_ANY, L"path" },
    { 0x12000004, S_ANY, L"description" },
    { 0x12000005, S_ANY, L"locale" },
    { 0x12000016, S_ANY, L"targetname" },
    { 0x12000019, S_ANY, L"busparams" },
    { 0x1200001d, S_ANY, L"key" },
    { 0x1200001f, S_ANY, L"hostipv6" },
    { 0x12000030, S_ANY, L"loadoptions" },
    { 0x12000044, S_ANY, L"bootstatfilepath" },
    { 0x1200004a, S_ANY, L"fontpath" },
    { 0x12000088, S_ANY, L"additionalcipolicy" },
    { 0x14000006, S_ANY, L"inherit" },
    { 0x14000008, S_ANY, L"recoverysequence" },
    { 0x15000007, S_ANY, L"truncatememory" },
    { 0x1500000c, S_ANY, L"firstmegabytepolicy" },
    { 0x1500000d, S_ANY, L"relocatephysical" },
    { 0x1500000e, S_ANY, L"avoidlowmemory" },
    { 0x15000011, S_ANY, L"debugtype" },
    { 0x15000012, S_ANY, L"debugaddress" },
    { 0x15000013, S_ANY, L"debugport" },
    { 0x15000014, S_ANY, L"baudrate" },
    { 0x15000015, S_ANY, L"channel" },
    { 0x15000018, S_ANY, L"debugstart" },
    { 0x1500001a, S_ANY, L"hostip" },
    { 0x1500001b, S_ANY, L"port" },
    { 0x15000021, S_ANY, L"targetip" },
    { 0x15000022, S_ANY, L"emsport" },
    { 0x15000023, S_ANY, L"emsbaudrate" },
    { 0x15000024, S_ANY, L"gatewayip" },
    { 0x15000042, S_ANY, L"keyringaddress" },
    { 0x15000047, S_ANY, L"configaccesspolicy" },
    { 0x1500004b, S_ANY, L"integrityservices" },
    { 0x1500004c, S_ANY, L"volumebandid" },
    { 0x15000051, S_ANY, L"initialconsoleinput" },
    { 0x15000052, S_ANY, L"graphicsresolution" },
    { 0x15000065, S_ANY, L"displaymessage" },
    { 0x15000066, S_ANY, L"displaymessageoverride" },
    { 0x15000079, S_ANY, L"bootuxtransitiontime" },
    { 0x1500007d, S_ANY, L"booterrorux" },
    { 0x1500007f, S_ANY, L"measuredbootlogformat" },
    { 0x15000080, S_ANY, L"displayrotation" },
    { 0x15000081, S_ANY, L"logcontrol" },
    { 0x15000088, S_ANY, L"linearaddress57" },
    { 0x1500008e, S_ANY, L"heapvalidatecount" },
    { 0x16000009, S_ANY, L"recoveryenabled" },
    { 0x1600000b, S_ANY, L"badmemoryaccess" },
    { 0x1600000f, S_ANY, L"traditionalkseg" },
    { 0x16000010, S_ANY, L"bootdebug" },
    { 0x16000017, S_ANY, L"noumex" },
    { 0x1600001c, S_ANY, L"dhcp" },
    { 0x1600001e, S_ANY, L"vm" },
    { 0x16000020, S_ANY, L"bootems" },
    { 0x16000025, S_ANY, L"enablejtagbreak" },
    { 0x16000040, S_ANY, L"advancedoptions" },
    { 0x16000041, S_ANY, L"optionsedit" },
    { 0x16000045, S_ANY, L"preservebootstat" },
    { 0x16000046, S_ANY, L"graphicsmodedisabled" },
    { 0x16000048, S_ANY, L"nointegritychecks" },
    { 0x16000049, S_ANY, L"testsigning" },
    { 0x16000050, S_ANY, L"extendedinput" },
    { 0x16000053, S_ANY, L"restartonfailure" },
    { 0x16000054, S_ANY, L"highestmode" },
    { 0x16000060, S_ANY, L"isolatedcontext" },
    { 0x16000068, S_ANY, L"nobootuxtext" },
    { 0x16000069, S_ANY, L"nobootuxprogress" },
    { 0x1600006a, S_ANY, L"nobootuxfade" },
    { 0x1600006c, S_ANY, L"bootuxdisabled" },
    { 0x16000072, S_ANY, L"nokeyboard" },
    { 0x16000074, S_ANY, L"bootshutdowndisabled" },
    { 0x1600007a, S_ANY, L"mobilegraphics" },
    { 0x1600007b, S_ANY, L"forcefipscrypto" },
    { 0x1600007e, S_ANY, L"flightsigning" },
    { 0x16000082, S_ANY, L"nofirmwaresync" },
    { 0x16000087, S_ANY, L"numlock" },
    { 0x1700000a, S_ANY, L"badmemorylist" },
    { 0x17000077, S_ANY, L"allowedinmemorysettings" },
    { 0x21000001, S_OSLOADER, L"osdevice" },
    { 0x21000001, S_RESUME, L"filedevice" },
    { 0x21000005, S_ANY, L"associatedosdevice" },
    { 0x21000022, S_ANY, L"bcddevice" },
    { 0x2100002b, S_ANY, L"alternatebootdevice" },
    { 0x21000138, S_ANY, L"imcdevice" },
    { 0x21000150, S_ANY, L"systemdatadevice" },
    { 0x21000151, S_ANY, L"osarcdevice" },
    { 0x21000153, S_ANY, L"osdatadevice" },
    { 0x21000154, S_ANY, L"bspdevice" },
    { 0x22000001, S_ANY, L"bpbstring" },
    { 0x22000002, S_OSLOADER, L"systemroot" },
    { 0x22000002, S_RESUME, L"filepath" },
    { 0x22000002, S_STARTUP, L"applicationname" },
    { 0x22000011, S_ANY, L"kernel" },
    { 0x22000012, S_ANY, L"hal" },
    { 0x22000013, S_ANY, L"dbgtransport" },
    { 0x22000023, S_ANY, L"bcdfilepath" },
    { 0x22000040, S_ANY, L"fverecoveryurl" },
    { 0x22000041, S_ANY, L"fverecoverymessage" },
    { 0x22000053, S_ANY, L"evstore" },
    { 0x220000f9, S_ANY, L"hypervisorbusparams" },
    { 0x22000110, S_ANY, L"hypervisorusekey" },
    { 0x22000117, S_ANY, L"hypervisorloadoptions" },
    { 0x2200011b, S_ANY, L"hypervisorrootprocnumanodes" },
    { 0x2200011e, S_ANY, L"hypervisorrootprocnumanodelps" },
    { 0x2200012d, S_ANY, L"kernelbusparams" },
    { 0x22000132, S_ANY, L"kerneltargetname" },
    { 0x22000136, S_ANY, L"kernelkey" },
    { 0x22000137, S_ANY, L"imchivename" },
    { 0x22000140, S_ANY, L"mfgmode" },
    { 0x22000155, S_ANY, L"bspfilepath" },
    { 0x22000156, S_ANY, L"kernelhostipv6" },
    { 0x22000161, S_ANY, L"hypervisorhostipv6" },
    { 0x23000003, S_BOOTMGR, L"default" },
    { 0x23000003, S_OSLOADER, L"resumeobject" },
    { 0x23000006, S_ANY, L"resumeobject" },
    { 0x24000001, S_ANY, L"displayorder" },
    { 0x24000002, S_ANY, L"bootsequence" },
    { 0x24000007, S_ANY, L"startupsequence" },
    { 0x24000010, S_ANY, L"toolsdisplayorder" },
    { 0x25000001, S_ANY, L"passcount" },
    { 0x25000002, S_ANY, L"testmix" },
    { 0x25000004, S_ANY, L"timeout" },
    { 0x25000005, S_ANY, L"stridefailcount" },
    { 0x25000006, S_ANY, L"invcfailcount" },
    { 0x25000007, S_MEMTEST, L"matsfailcount" },
    { 0x25000007, S_RESUME, L"bootux" },
    { 0x25000008, S_MEMTEST, L"randfailcount" },
    { 0x25000008, S_RESUME, L"bootmenupolicy" },
    { 0x25000009, S_ANY, L"chckrfailcount" },
    { 0x25000020, S_ANY, L"nx" },
    { 0x25000021, S_ANY, L"pae" },
    { 0x25000031, S_ANY, L"removememory" },
    { 0x25000032, S_ANY, L"increaseuserva" },
    { 0x25000033, S_ANY, L"perfmem" },
    { 0x25000050, S_ANY, L"clustermodeaddressing" },
    { 0x25000052, S_ANY, L"restrictapiccluster" },
    { 0x25000055, S_ANY, L"x2apicpolicy" },
    { 0x25000061, S_ANY, L"numproc" },
    { 0x25000063, S_ANY, L"configflags" },
    { 0x25000066, S_ANY, L"groupsize" },
    { 0x25000071, S_ANY, L"msi" },
    { 0x25000072, S_ANY, L"pciexpress" },
    { 0x25000080, S_ANY, L"safeboot" },
    { 0x250000a6, S_ANY, L"tscsyncpolicy" },
    { 0x250000c1, S_ANY, L"driverloadfailurepolicy" },
    { 0x250000c2, S_ANY, L"bootmenupolicy" },
    { 0x250000e0, S_ANY, L"bootstatuspolicy" },
    { 0x250000f0, S_ANY, L"hypervisorlaunchtype" },
    { 0x250000f3, S_ANY, L"hypervisordebugtype" },
    { 0x250000f4, S_ANY, L"hypervisordebugport" },
    { 0x250000f5, S_ANY, L"hypervisorbaudrate" },
    { 0x250000f6, S_ANY, L"hypervisorchannel" },
    { 0x250000f7, S_ANY, L"bootux" },
    { 0x250000fa, S_ANY, L"hypervisornumproc" },
    { 0x250000fb, S_ANY, L"hypervisorrootprocpernode" },
    { 0x250000fd, S_ANY, L"hypervisorhostip" },
    { 0x250000fe, S_ANY, L"hypervisorhostport" },
    { 0x250000ff, S_ANY, L"hypervisordebugpages" },
    { 0x25000100, S_ANY, L"tpmbootentropy" },
    { 0x25000113, S_ANY, L"hypervisorrootproc" },
    { 0x25000115, S_ANY, L"hypervisoriommupolicy" },
    { 0x25000118, S_ANY, L"hypervisormsrfilterpolicy" },
    { 0x25000119, S_ANY, L"hypervisormmionxpolicy" },
    { 0x2500011a, S_ANY, L"hypervisorschedulertype" },
    { 0x2500011c, S_ANY, L"hypervisorperfmon" },
    { 0x2500011d, S_ANY, L"hypervisorrootprocpercore" },
    { 0x25000120, S_ANY, L"xsavepolicy" },
    { 0x25000121, S_ANY, L"xsaveaddfeature0" },
    { 0x25000122, S_ANY, L"xsaveaddfeature1" },
    { 0x25000123, S_ANY, L"xsaveaddfeature2" },
    { 0x25000124, S_ANY, L"xsaveaddfeature3" },
    { 0x25000125, S_ANY, L"xsaveaddfeature4" },
    { 0x25000126, S_ANY, L"xsaveaddfeature5" },
    { 0x25000127, S_ANY, L"xsaveaddfeature6" },
    { 0x25000128, S_ANY, L"xsaveaddfeature7" },
    { 0x25000129, S_ANY, L"xsaveremovefeature" },
    { 0x2500012a, S_ANY, L"xsaveprocessorsmask" },
    { 0x2500012b, S_ANY, L"xsavedisable" },
    { 0x2500012c, S_ANY, L"kerneldebugtype" },
    { 0x2500012e, S_ANY, L"kerneldebugaddress" },
    { 0x2500012f, S_ANY, L"kerneldebugport" },
    { 0x25000130, S_ANY, L"claimedtpmcounter" },
    { 0x25000131, S_ANY, L"kernelchannel" },
    { 0x25000133, S_ANY, L"kernelhostip" },
    { 0x25000134, S_ANY, L"kernelport" },
    { 0x25000139, S_ANY, L"kernelbaudrate" },
    { 0x25000142, S_ANY, L"vsmlaunchtype" },
    { 0x25000144, S_ANY, L"hypervisorenforcedcodeintegrity" },
    { 0x25000158, S_ANY, L"kerneltargetip" },
    { 0x25000159, S_ANY, L"kernelgatewayip" },
    { 0x25000162, S_ANY, L"hypervisortargetip" },
    { 0x25000163, S_ANY, L"hypervisorgatewayip" },
    { 0x25000510, S_ANY, L"chargethreshold" },
    { 0x25000aaa, S_ANY, L"bootflow" },
    { 0x26000001, S_ANY, L"pxesoftreboot" },
    { 0x26000003, S_MEMTEST, L"cacheenable" },
    { 0x26000003, S_RESUME, L"customsettings" },
    { 0x26000004, S_MEMTEST, L"failuresenabled" },
    { 0x26000004, S_RESUME, L"pae" },
    { 0x26000005, S_ANY, L"resume" },
    { 0x26000006, S_ANY, L"debugoptionenabled" },
    { 0x26000010, S_ANY, L"detecthal" },
    { 0x26000020, S_ANY, L"displaybootmenu" },
    { 0x26000021, S_ANY, L"noerrordisplay" },
    { 0x26000022, S_ANY, L"winpe" },
    { 0x26000024, S_ANY, L"nocrashautoreboot" },
    { 0x26000025, S_BOOTMGR, L"hiberboot" },
    { 0x26000025, S_OSLOADER, L"lastknowngood" },
    { 0x26000028, S_ANY, L"processcustomactionsfirst" },
    { 0x2600002c, S_ANY, L"cachebootbcd" },
    { 0x26000030, S_ANY, L"nolowmem" },
    { 0x26000031, S_ANY, L"persistbootsequence" },
    { 0x26000032, S_ANY, L"skipstartupsequence" },
    { 0x26000033, S_ANY, L"nextentryonfailure" },
    { 0x26000040, S_ANY, L"vga" },
    { 0x26000041, S_ANY, L"quietboot" },
    { 0x26000042, S_BOOTMGR, L"flightedbootmgr" },
    { 0x26000042, S_OSLOADER, L"novesa" },
    { 0x26000043, S_ANY, L"novga" },
    { 0x26000051, S_ANY, L"usephysicaldestination" },
    { 0x26000054, S_ANY, L"uselegacyapicmode" },
    { 0x26000060, S_ANY, L"onecpu" },
    { 0x26000062, S_ANY, L"maxproc" },
    { 0x26000064, S_ANY, L"maxgroup" },
    { 0x26000065, S_ANY, L"groupaware" },
    { 0x26000070, S_ANY, L"usefirmwarepcisettings" },
    { 0x26000081, S_ANY, L"safebootalternateshell" },
    { 0x26000090, S_ANY, L"bootlog" },
    { 0x26000091, S_ANY, L"sos" },
    { 0x260000a0, S_ANY, L"debug" },
    { 0x260000a1, S_ANY, L"halbreakpoint" },
    { 0x260000a2, S_ANY, L"useplatformclock" },
    { 0x260000a3, S_ANY, L"forcelegacyplatform" },
    { 0x260000a4, S_ANY, L"useplatformtick" },
    { 0x260000a5, S_ANY, L"disabledynamictick" },
    { 0x260000b0, S_ANY, L"ems" },
    { 0x260000c3, S_ANY, L"onetimeadvancedoptions" },
    { 0x260000c4, S_ANY, L"onetimeoptionsedit" },
    { 0x260000e1, S_ANY, L"disableelamdrivers" },
    { 0x260000f2, S_ANY, L"hypervisordebug" },
    { 0x260000f8, S_ANY, L"hypervisordisableslat" },
    { 0x260000fc, S_ANY, L"hypervisoruselargevtlb" },
    { 0x26000114, S_ANY, L"hypervisordhcp" },
    { 0x26000116, S_ANY, L"hypervisorusevapic" },
    { 0x26000135, S_ANY, L"kerneldhcp" },
    { 0x26000141, S_ANY, L"event" },
    { 0x26000145, S_MOBILE, L"enablebootdebugpolicy" },
    { 0x26000145, S_OSLOADER, L"dtrace" },
    { 0x26000146, S_ANY, L"enablebootorderclean" },
    { 0x26000147, S_ANY, L"enabledeviceid" },
    { 0x26000148, S_ANY, L"enableffuloader" },
    { 0x26000149, S_ANY, L"enableiuloader" },
    { 0x2600014a, S_ANY, L"enablemassstorage" },
    { 0x2600014b, S_ANY, L"enablerpmbprovisioning" },
    { 0x2600014c, S_ANY, L"enablesecurebootpolicy" },
    { 0x2600014d, S_ANY, L"enablestartcharge" },
    { 0x2600014e, S_ANY, L"enableresettpm" },
    { 0x2600015b, S_ANY, L"nokdnic" },
    { 0x26000202, S_ANY, L"skipffumode" },
    { 0x26000203, S_ANY, L"forceffumode" },
    { 0x26000512, S_ANY, L"offmodecharging" },
    { 0x27000030, S_ANY, L"customactions" },
    { 0x31000003, S_ANY, L"ramdisksdidevice" },
    { 0x3100000e, S_ANY, L"primarydevice" },
    { 0x3100000f, S_ANY, L"secondarydevice" },
    { 0x31000010, S_ANY, L"spacesdiskdevice" },
    { 0x31000012, S_ANY, L"cimfsparentdevice" },
    { 0x32000004, S_ANY, L"ramdisksdipath" },
    { 0x32000011, S_ANY, L"cimfsrootdirectory" },
    { 0x35000001, S_ANY, L"ramdiskimageoffset" },
    { 0x35000002, S_ANY, L"ramdisktftpclientport" },
    { 0x35000005, S_ANY, L"ramdiskimagelength" },
    { 0x35000007, S_ANY, L"ramdisktftpblocksize" },
    { 0x35000008, S_ANY, L"ramdisktftpwindowsize" },
    { 0x3500000d, S_ANY, L"vhdramdiskexpansion" },
    { 0x35000020, S_ANY, L"memdisksize" },
    { 0x36000006, S_ANY, L"exportascd" },
    { 0x36000009, S_ANY, L"ramdiskmcenabled" },
    { 0x3600000a, S_ANY, L"ramdiskmctftpfallback" },
    { 0x3600000b, S_ANY, L"ramdisktftpvarwindow" },
    { 0x3600000c, S_ANY, L"vhdramdiskboot" },
    { 0x46000007, S_ANY, L"migrateallelements" },
    { 0x47000006, S_ANY, L"elementstomigrate" },
};

/* Object Type value -> heading resource id. */
static const struct { ULONG Type; UINT Heading; } g_ObjectHeadings[] =
{
    { 0x10100001, IDS_HDR_FWBOOTMGR },
    { 0x10100002, IDS_HDR_BOOTMGR },
    { 0x10200005, IDS_HDR_MEMDIAG },
    { 0x10200004, IDS_HDR_RESUME },
    { 0x10200003, IDS_HDR_BOOTLOADER },
    { 0x10300006, IDS_HDR_NTLDR },
    { 0x10400008, IDS_HDR_BOOTSECTOR },
    { 0x10200006, IDS_HDR_STARTUP },
    { 0x20100000, IDS_HDR_GLOBALSETTINGS },
    { 0x20200003, IDS_HDR_BOOTLOADERSET },
    { 0x20200004, IDS_HDR_RESUMESET },
    { 0x20100001, IDS_HDR_DBGSETTINGS },
    { 0x20100002, IDS_HDR_EMSSETTINGS },
    { 0x20100003, IDS_HDR_BADMEMORY },
    { 0x30000000, IDS_HDR_DEVOPTIONS },
};

static
VOID
PrintResource(
    _In_ UINT Id)
{
    WCHAR Buffer[1024];

    if (LoadStringW(GetModuleHandleW(NULL), Id, Buffer, ARRAYSIZE(Buffer)) > 0)
        fputws(Buffer, stdout);
}

static
PCWSTR
AliasForId(
    _In_ PCWSTR Guid)
{
    SIZE_T i;

    for (i = 0; i < ARRAYSIZE(g_WellKnownIds); i++)
    {
        if (_wcsicmp(Guid, g_WellKnownIds[i].Guid) == 0)
            return g_WellKnownIds[i].Alias;
    }
    return Guid;
}

static
PCWSTR
NameForElement(
    _In_ ULONG Type,
    _In_ BCD_SCOPE Scope,
    _Out_writes_z_(Chars) PWSTR Custom,
    _In_ SIZE_T Chars)
{
    PCWSTR Fallback = NULL;
    SIZE_T i;

    for (i = 0; i < ARRAYSIZE(g_ElementNames); i++)
    {
        if (g_ElementNames[i].Type != Type)
            continue;
        if (g_ElementNames[i].Scope == Scope)
            return g_ElementNames[i].Name;
        if (g_ElementNames[i].Scope == S_ANY)
            Fallback = g_ElementNames[i].Name;
    }
    if (Fallback != NULL)
        return Fallback;
    StringCchPrintfW(Custom, Chars, L"custom:%08lx", Type);
    return Custom;
}

static
UINT
HeadingForType(
    _In_ ULONG Type)
{
    SIZE_T i;

    for (i = 0; i < ARRAYSIZE(g_ObjectHeadings); i++)
    {
        if (g_ObjectHeadings[i].Type == Type)
            return g_ObjectHeadings[i].Heading;
    }
    return IDS_HDR_UNKNOWN;
}

/* The object class picks which name a shared element id prints under. Match the whole application
 * Type, like the heading table, so the image-type nibble is not ignored: 0x10200006 (startup) and
 * 0x10300006 (ntldr) are different classes. A class with no application-specific shared elements
 * (ntldr, boot sector, settings, device, unknown) falls to S_ANY, which carries library elements. */
static
BCD_SCOPE
ScopeForObjectType(
    _In_ ULONG Type)
{
    switch (Type)
    {
        case 0x10100001:    /* {fwbootmgr} */
        case 0x10100002:    /* {bootmgr} */
            return S_BOOTMGR;
        case 0x10200003:    /* OS loader */
            return S_OSLOADER;
        case 0x10200004:    /* resume */
            return S_RESUME;
        case 0x10200005:    /* memory diagnostic */
            return S_MEMTEST;
        case 0x10200006:    /* Windows setup / startup */
            return S_STARTUP;
        default:
            return S_ANY;
    }
}

static
ULONG
LeDword(
    _In_reads_bytes_(4) const BYTE *Data)
{
    return (ULONG)Data[0] | ((ULONG)Data[1] << 8) | ((ULONG)Data[2] << 16) | ((ULONG)Data[3] << 24);
}

/* Print "name" padded to the field column; the caller adds the value. */
static
VOID
PrintFieldName(
    _In_ PCWSTR Name)
{
    int Pad = BCD_FIELD_WIDTH - (int)wcslen(Name);

    fputws(Name, stdout);
    while (Pad-- > 0)
        fputwc(L' ', stdout);
}

/* The 16-byte GUID as it is stored (Data1/2/3 little-endian, Data4 as bytes). */
static
VOID
PrintGuidBytes(
    _In_reads_bytes_(16) const BYTE *Guid)
{
    wprintf(L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            LeDword(Guid),
            (unsigned)(Guid[4] | (Guid[5] << 8)),
            (unsigned)(Guid[6] | (Guid[7] << 8)),
            Guid[8], Guid[9], Guid[10], Guid[11], Guid[12], Guid[13], Guid[14], Guid[15]);
}

/* A REG_MULTI_SZ of object ids, one per line, aligned under the value column. */
static
VOID
PrintGuidList(
    _In_ PCWSTR MultiSz)
{
    PCWSTR Id = MultiSz;
    BOOL First = TRUE;

    while (*Id)
    {
        if (!First)
        {
            fputwc(L'\n', stdout);
            PrintFieldName(L"");
        }
        fputws(AliasForId(Id), stdout);
        First = FALSE;
        Id += wcslen(Id) + 1;
    }
    fputwc(L'\n', stdout);
}

static
VOID
PrintDevice(
    _In_reads_bytes_(Length) const BYTE *Data,
    _In_ ULONG Length)
{
    const BYTE *Dev;
    ULONG Type;

    /* Need the device type and the disk sub-type at least. */
    if (Length < BCD_ELEMENT_DEVICE_OFFSET + BCD_DISK_TYPE_OFFSET + 4)
    {
        fputws(L"unknown\n", stdout);
        return;
    }
    Dev = Data + BCD_ELEMENT_DEVICE_OFFSET;
    Type = LeDword(Dev);

    switch (Type)
    {
        case BCD_DEVICE_BOOT:
            fputws(L"boot\n", stdout);
            return;

        case BCD_DEVICE_DISK:
            if (LeDword(Dev + BCD_DISK_TYPE_OFFSET) == BCD_DISK_RAMDISK)
                fputws(L"ramdisk\n", stdout);
            else
                fputws(L"disk\n", stdout);
            return;

        case BCD_DEVICE_PARTITION:
            if (Length < BCD_ELEMENT_DEVICE_OFFSET + BCD_DEVICE_DISK_OFFSET + 16)
            {
                fputws(L"partition\n", stdout);
                return;
            }
            if (LeDword(Dev + BCD_DEVICE_STYLE_OFFSET) == BCD_PARTITION_MBR)
            {
                wprintf(L"partition=mbr,signature=%08lx,offset=%08lx%08lx\n",
                        LeDword(Dev + BCD_DEVICE_SIGNATURE_OFFSET),
                        LeDword(Dev + BCD_DEVICE_PARTITION_OFFSET + 4),
                        LeDword(Dev + BCD_DEVICE_PARTITION_OFFSET));
            }
            else
            {
                fputws(L"partition=gpt,disk=", stdout);
                PrintGuidBytes(Dev + BCD_DEVICE_DISK_OFFSET);
                fputws(L",partition=", stdout);
                PrintGuidBytes(Dev + BCD_DEVICE_PARTITION_OFFSET);
                fputwc(L'\n', stdout);
            }
            return;

        default:
            wprintf(L"device type %lu\n", Type);
            return;
    }
}

/*
 * Read Elements\<type>\Element and print it under its field name. The value may be untrusted
 * (a /store file), unterminated, or larger than a fixed buffer: size it, add room for a double
 * NUL, terminate it, and only walk a string/list when its registry type matches the format.
 */
static
BOOL
PrintElement(
    _In_ HKEY Elements,
    _In_ ULONG Type,
    _In_ BCD_SCOPE Scope)
{
    WCHAR Sub[16], Custom[32];
    HKEY Key;
    BYTE *Data;
    DWORD Length = 0, RegType = 0;
    ULONG Format = BCD_ELEMENT_FORMAT(Type);
    LONG Error;

    StringCchPrintfW(Sub, ARRAYSIZE(Sub), L"%08lx", Type);
    if (RegOpenKeyExW(Elements, Sub, 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return FALSE;

    Error = RegQueryValueExW(Key, L"Element", NULL, &RegType, NULL, &Length);
    if (Error != ERROR_SUCCESS)
    {
        RegCloseKey(Key);
        return FALSE;
    }
    /* The value is present but empty; show the field with no value rather than hide it. */
    if (Length == 0)
    {
        RegCloseKey(Key);
        PrintFieldName(NameForElement(Type, Scope, Custom, ARRAYSIZE(Custom)));
        fputwc(L'\n', stdout);
        return TRUE;
    }

    Data = HeapAlloc(GetProcessHeap(), 0, Length + 2 * sizeof(WCHAR));
    if (Data == NULL)
    {
        RegCloseKey(Key);
        return FALSE;
    }

    Error = RegQueryValueExW(Key, L"Element", NULL, &RegType, Data, &Length);
    RegCloseKey(Key);
    if (Error != ERROR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, Data);
        return FALSE;
    }
    /* Guarantee a double-NUL terminator past the data for string and list walks. */
    Data[Length] = 0;
    Data[Length + 1] = 0;
    Data[Length + 2] = 0;
    Data[Length + 3] = 0;

    PrintFieldName(NameForElement(Type, Scope, Custom, ARRAYSIZE(Custom)));

    switch (Format)
    {
        case BCD_ELEMENT_FORMAT_STRING:
            if (RegType == REG_SZ)
                fputws((PCWSTR)Data, stdout);
            fputwc(L'\n', stdout);
            break;
        case BCD_ELEMENT_FORMAT_OBJECT:
            if (RegType == REG_SZ)
                fputws(AliasForId((PCWSTR)Data), stdout);
            fputwc(L'\n', stdout);
            break;
        case BCD_ELEMENT_FORMAT_OBJECTLIST:
            if (RegType == REG_MULTI_SZ)
                PrintGuidList((PCWSTR)Data);
            else
                fputwc(L'\n', stdout);
            break;
        case BCD_ELEMENT_FORMAT_BOOLEAN:
            fputws((Length >= 1 && Data[0]) ? L"Yes\n" : L"No\n", stdout);
            break;
        case BCD_ELEMENT_FORMAT_INTEGER:
        case BCD_ELEMENT_FORMAT_INTEGERLIST:
        {
            ULONGLONG Value = 0;
            DWORD i;
            for (i = 0; i < Length && i < 8; i++)
                Value |= (ULONGLONG)Data[i] << (8 * i);
            wprintf(L"%llu\n", Value);
            break;
        }
        case BCD_ELEMENT_FORMAT_DEVICE:
            PrintDevice(Data, Length);
            break;
        default:
            fputwc(L'\n', stdout);
            break;
    }

    HeapFree(GetProcessHeap(), 0, Data);
    return TRUE;
}

static
int
__cdecl
CompareUlong(
    _In_ const void *A,
    _In_ const void *B)
{
    ULONG Ua = *(const ULONG *)A, Ub = *(const ULONG *)B;

    if (Ua < Ub) return -1;
    if (Ua > Ub) return 1;
    return 0;
}

/* A valid element subkey name is exactly eight hex digits; anything else is not an element. */
static
BOOL
IsHexName8(
    _In_ PCWSTR Name,
    _In_ DWORD Chars)
{
    DWORD k;

    if (Chars != 8)
        return FALSE;
    for (k = 0; k < 8; k++)
    {
        WCHAR c = Name[k];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')))
            return FALSE;
    }
    return TRUE;
}

static
VOID
PrintObject(
    _In_ HKEY Objects,
    _In_ PCWSTR Id)
{
    HKEY Object, Description, Elements;
    WCHAR Heading[128];
    DWORD Type = 0, Length = sizeof(Type), RegType = 0, Index;
    ULONG Types[BCD_MAX_ELEMENTS], Count = 0;
    BCD_SCOPE Scope;
    LONG Error;
    SIZE_T i;

    if (RegOpenKeyExW(Objects, Id, 0, KEY_READ, &Object) != ERROR_SUCCESS)
        return;

    if (RegOpenKeyExW(Object, L"Description", 0, KEY_READ, &Description) == ERROR_SUCCESS)
    {
        RegQueryValueExW(Description, L"Type", NULL, &RegType, (BYTE *)&Type, &Length);
        RegCloseKey(Description);
    }
    Scope = ScopeForObjectType(Type);

    if (LoadStringW(GetModuleHandleW(NULL), HeadingForType(Type), Heading, ARRAYSIZE(Heading)) > 0)
    {
        int Dashes = (int)wcslen(Heading);
        fputws(Heading, stdout);
        fputwc(L'\n', stdout);
        while (Dashes-- > 0)
            fputwc(L'-', stdout);
        fputwc(L'\n', stdout);
    }

    PrintFieldName(L"identifier");
    fputws(AliasForId(Id), stdout);
    fputwc(L'\n', stdout);

    if (RegOpenKeyExW(Object, L"Elements", 0, KEY_READ, &Elements) == ERROR_SUCCESS)
    {
        /* Enumerate every element present, not only the named ones, so nothing is hidden. */
        for (Index = 0; ; Index++)
        {
            WCHAR Name[16];
            DWORD Chars = ARRAYSIZE(Name);

            Error = RegEnumKeyExW(Elements, Index, Name, &Chars, NULL, NULL, NULL, NULL);
            if (Error == ERROR_NO_MORE_ITEMS)
                break;
            /* Skip anything that is not an element and keep going rather than stop the whole
             * listing: an enumeration error (a name too long for the buffer is ERROR_MORE_DATA)
             * or a subkey name that is not exactly eight hex digits. */
            if (Error != ERROR_SUCCESS || !IsHexName8(Name, Chars))
                continue;
            if (Count == ARRAYSIZE(Types))
            {
                fwprintf(stderr, L"bcdedit: too many elements, the listing is truncated\n");
                break;
            }
            Types[Count++] = wcstoul(Name, NULL, 16);
        }
        qsort(Types, Count, sizeof(ULONG), CompareUlong);
        for (i = 0; i < Count; i++)
            PrintElement(Elements, Types[i], Scope);
        RegCloseKey(Elements);
    }

    fputwc(L'\n', stdout);
    RegCloseKey(Object);
}

static
LONG
OpenStore(
    _In_opt_ PCWSTR StoreFile,
    _In_ REGSAM Access,
    _Out_ PHKEY Store)
{
    if (StoreFile == NULL)
        return RegOpenKeyExW(HKEY_LOCAL_MACHINE, BCD_SYSTEM_STORE, 0, Access, Store);

    return RegLoadAppKeyW(StoreFile, Store, Access, 0, 0);
}

static
BOOL
EnumStore(
    _In_opt_ PCWSTR StoreFile)
{
    HKEY Store, Objects;
    DWORD Index;
    LONG Error;

    Error = OpenStore(StoreFile, KEY_READ, &Store);
    if (Error != ERROR_SUCCESS)
    {
        PrintResource(IDS_ERR_OPENSTORE);
        return FALSE;
    }

    Error = RegOpenKeyExW(Store, L"Objects", 0, KEY_READ, &Objects);
    if (Error != ERROR_SUCCESS)
    {
        RegCloseKey(Store);
        PrintResource(IDS_ERR_OPENSTORE);
        return FALSE;
    }

    for (Index = 0; ; Index++)
    {
        WCHAR Name[64];
        DWORD Chars = ARRAYSIZE(Name);

        Error = RegEnumKeyExW(Objects, Index, Name, &Chars, NULL, NULL, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            break;
        PrintObject(Objects, Name);
    }

    RegCloseKey(Objects);
    RegCloseKey(Store);
    return TRUE;
}

/* Resolve an alias ({bootmgr}) to its stored id; any other value (a raw {guid}) is used as is. */
static
PCWSTR
IdForAlias(
    _In_ PCWSTR Name,
    _Out_writes_z_(Chars) PWSTR Buffer,
    _In_ SIZE_T Chars)
{
    SIZE_T i;

    for (i = 0; i < ARRAYSIZE(g_WellKnownIds); i++)
    {
        if (_wcsicmp(Name, g_WellKnownIds[i].Alias) == 0)
            return g_WellKnownIds[i].Guid;
    }
    StringCchCopyW(Buffer, Chars, Name);
    return Buffer;
}

static
BOOL
IsHexDigit(
    _In_ WCHAR c)
{
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

/* The {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} form, so a value with a backslash or an
 * unresolved alias is not taken as a key path or a real id. */
static
BOOL
IsValidGuid(
    _In_ PCWSTR Id)
{
    SIZE_T i;

    if (wcslen(Id) != 38 || Id[0] != L'{' || Id[37] != L'}')
        return FALSE;
    for (i = 1; i <= 36; i++)
    {
        if (i == 9 || i == 14 || i == 19 || i == 24)
        {
            if (Id[i] != L'-')
                return FALSE;
        }
        else if (!IsHexDigit(Id[i]))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/* Decimal, or hex with a 0x prefix; no sign, like Windows bcdedit. */
static
BOOL
ParseInteger(
    _In_ PCWSTR String,
    _Out_ PULONGLONG Value)
{
    WCHAR *End;
    int Base = 10;

    /* A digit must lead. This rejects a sign, empty input, and the leading whitespace that
     * _wcstoui64 would otherwise skip over. */
    if (String[0] < L'0' || String[0] > L'9')
        return FALSE;
    if (String[0] == L'0' && (String[1] == L'x' || String[1] == L'X'))
        Base = 16;
    errno = 0;
    *Value = _wcstoui64(String, &End, Base);
    if (errno == ERANGE)
        return FALSE;
    return (End != String && *End == 0);
}

/* Map an element name (timeout), a custom:xxxxxxxx, or a raw 8-hex type to its identifier. A name
 * shared by more than one class (bootmenupolicy, resumeobject) resolves by the object's scope. */
static
BOOL
TypeForName(
    _In_ PCWSTR Name,
    _In_ BCD_SCOPE Scope,
    _Out_ PULONG Type)
{
    BOOL HaveFallback = FALSE;
    ULONG Fallback = 0;
    SIZE_T i;
    PCWSTR Digits = Name;
    WCHAR *End;

    for (i = 0; i < ARRAYSIZE(g_ElementNames); i++)
    {
        if (_wcsicmp(Name, g_ElementNames[i].Name) != 0)
            continue;
        if (g_ElementNames[i].Scope == Scope)
        {
            *Type = g_ElementNames[i].Type;
            return TRUE;
        }
        if (g_ElementNames[i].Scope == S_ANY)
        {
            Fallback = g_ElementNames[i].Type;
            HaveFallback = TRUE;
        }
    }
    if (HaveFallback)
    {
        *Type = Fallback;
        return TRUE;
    }
    if (_wcsnicmp(Name, L"custom:", 7) == 0)
        Digits = Name + 7;
    if (wcslen(Digits) != 8)
        return FALSE;
    for (i = 0; i < 8; i++)
    {
        if (!IsHexDigit(Digits[i]))
            return FALSE;
    }
    *Type = wcstoul(Digits, &End, 16);
    return TRUE;
}

static
BOOL
ParseBoolean(
    _In_ PCWSTR Value,
    _Out_ PBYTE Out)
{
    if (_wcsicmp(Value, L"yes") == 0 || _wcsicmp(Value, L"on") == 0 ||
        _wcsicmp(Value, L"true") == 0 || _wcsicmp(Value, L"1") == 0)
    {
        *Out = 1;
        return TRUE;
    }
    if (_wcsicmp(Value, L"no") == 0 || _wcsicmp(Value, L"off") == 0 ||
        _wcsicmp(Value, L"false") == 0 || _wcsicmp(Value, L"0") == 0)
    {
        *Out = 0;
        return TRUE;
    }
    return FALSE;
}

/* Open or create Objects\<id>\Elements\<type> on an existing object and set its Element value. */
static
BOOL
WriteElement(
    _In_ HKEY Store,
    _In_ PCWSTR Id,
    _In_ ULONG Type,
    _In_ DWORD RegType,
    _In_reads_bytes_(Length) const void *Data,
    _In_ DWORD Length)
{
    HKEY Objects, Object, Elements, Element;
    WCHAR Sub[16];
    BOOL Ok = FALSE;

    if (RegOpenKeyExW(Store, L"Objects", 0, KEY_READ, &Objects) != ERROR_SUCCESS)
        return FALSE;
    if (RegOpenKeyExW(Objects, Id, 0, KEY_READ | KEY_WRITE, &Object) != ERROR_SUCCESS)
    {
        RegCloseKey(Objects);
        return FALSE;   /* the object must already exist; /create adds one */
    }
    if (RegCreateKeyExW(Object, L"Elements", 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &Elements, NULL) == ERROR_SUCCESS)
    {
        StringCchPrintfW(Sub, ARRAYSIZE(Sub), L"%08lx", Type);
        if (RegCreateKeyExW(Elements, Sub, 0, NULL, 0, KEY_WRITE, NULL, &Element, NULL) == ERROR_SUCCESS)
        {
            Ok = (RegSetValueExW(Element, L"Element", 0, RegType, (const BYTE *)Data, Length) == ERROR_SUCCESS);
            RegCloseKey(Element);
        }
        RegCloseKey(Elements);
    }
    RegCloseKey(Object);
    RegCloseKey(Objects);
    return Ok;
}

static
BOOL
DeleteElement(
    _In_ HKEY Store,
    _In_ PCWSTR Id,
    _In_ ULONG Type)
{
    HKEY Objects, Object, Elements;
    WCHAR Sub[16];
    BOOL Ok = FALSE;

    if (RegOpenKeyExW(Store, L"Objects", 0, KEY_READ, &Objects) != ERROR_SUCCESS)
        return FALSE;
    if (RegOpenKeyExW(Objects, Id, 0, KEY_READ, &Object) != ERROR_SUCCESS)
    {
        RegCloseKey(Objects);
        return FALSE;
    }
    if (RegOpenKeyExW(Object, L"Elements", 0, KEY_READ | KEY_WRITE, &Elements) == ERROR_SUCCESS)
    {
        StringCchPrintfW(Sub, ARRAYSIZE(Sub), L"%08lx", Type);
        Ok = (RegDeleteKeyW(Elements, Sub) == ERROR_SUCCESS);
        RegCloseKey(Elements);
    }
    RegCloseKey(Object);
    RegCloseKey(Objects);
    return Ok;
}

/* Encode one or more argument words for Type and write them. Device values are not set yet. */
static
BOOL
SetValue(
    _In_ HKEY Store,
    _In_ PCWSTR Id,
    _In_ ULONG Type,
    _In_ int Argc,
    _In_reads_(Argc) WCHAR **Argv,
    _In_ int First)
{
    switch (BCD_ELEMENT_FORMAT(Type))
    {
        case BCD_ELEMENT_FORMAT_STRING:
            if (First >= Argc)
                return FALSE;
            return WriteElement(Store, Id, Type, REG_SZ, Argv[First],
                                (DWORD)((wcslen(Argv[First]) + 1) * sizeof(WCHAR)));

        case BCD_ELEMENT_FORMAT_OBJECT:
        {
            WCHAR Buffer[64];
            PCWSTR Guid;

            if (First >= Argc)
                return FALSE;
            Guid = IdForAlias(Argv[First], Buffer, ARRAYSIZE(Buffer));
            if (!IsValidGuid(Guid))
                return FALSE;
            return WriteElement(Store, Id, Type, REG_SZ, Guid,
                                (DWORD)((wcslen(Guid) + 1) * sizeof(WCHAR)));
        }

        case BCD_ELEMENT_FORMAT_OBJECTLIST:
        {
            WCHAR Multi[2048], Buffer[64];
            SIZE_T Off = 0;
            int i;

            if (First >= Argc)
                return FALSE;
            for (i = First; i < Argc; i++)
            {
                PCWSTR Guid = IdForAlias(Argv[i], Buffer, ARRAYSIZE(Buffer));
                SIZE_T Chars = wcslen(Guid);

                if (!IsValidGuid(Guid))
                    return FALSE;
                if (Off + Chars + 2 > ARRAYSIZE(Multi))
                    return FALSE;
                memcpy(Multi + Off, Guid, (Chars + 1) * sizeof(WCHAR));
                Off += Chars + 1;
            }
            Multi[Off++] = 0;   /* the trailing NUL of the double-NUL terminator */
            return WriteElement(Store, Id, Type, REG_MULTI_SZ, Multi, (DWORD)(Off * sizeof(WCHAR)));
        }

        case BCD_ELEMENT_FORMAT_INTEGER:
        {
            BYTE Bytes[8];
            ULONGLONG Value;
            int i;

            if (First >= Argc || !ParseInteger(Argv[First], &Value))
                return FALSE;
            for (i = 0; i < 8; i++)
            {
                Bytes[i] = (BYTE)(Value & 0xFF);
                Value >>= 8;
            }
            return WriteElement(Store, Id, Type, REG_BINARY, Bytes, sizeof(Bytes));
        }

        case BCD_ELEMENT_FORMAT_BOOLEAN:
        {
            BYTE Bool;

            if (First >= Argc || !ParseBoolean(Argv[First], &Bool))
                return FALSE;
            return WriteElement(Store, Id, Type, REG_BINARY, &Bool, 1);
        }

        default:
            return FALSE;   /* device and integer-list values are set in a later change */
    }
}

/* The scope of an existing object, so an element name is resolved against the right class. */
static
BCD_SCOPE
ScopeForStoreObject(
    _In_ HKEY Store,
    _In_ PCWSTR Id)
{
    HKEY Objects, Object, Description;
    DWORD Type = 0, Length = sizeof(Type), RegType = 0;

    if (RegOpenKeyExW(Store, L"Objects", 0, KEY_READ, &Objects) == ERROR_SUCCESS)
    {
        if (RegOpenKeyExW(Objects, Id, 0, KEY_READ, &Object) == ERROR_SUCCESS)
        {
            if (RegOpenKeyExW(Object, L"Description", 0, KEY_READ, &Description) == ERROR_SUCCESS)
            {
                RegQueryValueExW(Description, L"Type", NULL, &RegType, (BYTE *)&Type, &Length);
                RegCloseKey(Description);
            }
            RegCloseKey(Object);
        }
        RegCloseKey(Objects);
    }
    return ScopeForObjectType(Type);
}

/* /set, /timeout and /default all edit one element; open the store writable and report. For /set
 * the element name is given and resolved against the object's class; the others pass a fixed type. */
static
int
EditElement(
    _In_opt_ PCWSTR StoreFile,
    _In_ PCWSTR IdArg,
    _In_opt_ PCWSTR ElementName,
    _In_ ULONG Type,
    _In_ int Argc,
    _In_reads_(Argc) WCHAR **Argv,
    _In_ int First)
{
    WCHAR Buffer[64];
    HKEY Store;
    PCWSTR Id;
    BOOL Ok;

    if (OpenStore(StoreFile, KEY_READ | KEY_WRITE, &Store) != ERROR_SUCCESS)
    {
        PrintResource(IDS_ERR_OPENSTORE);
        return 1;
    }
    Id = IdForAlias(IdArg, Buffer, ARRAYSIZE(Buffer));
    if (!IsValidGuid(Id))
    {
        RegCloseKey(Store);
        PrintResource(IDS_ERR_BADID);
        return 1;
    }
    if (ElementName != NULL && !TypeForName(ElementName, ScopeForStoreObject(Store, Id), &Type))
    {
        RegCloseKey(Store);
        PrintResource(IDS_ERR_BADPARAM);
        return 1;
    }
    Ok = SetValue(Store, Id, Type, Argc, Argv, First);
    RegCloseKey(Store);

    PrintResource(Ok ? IDS_OK : IDS_ERR_SETFAIL);
    return Ok ? 0 : 1;
}

static
int
DeleteValueCommand(
    _In_opt_ PCWSTR StoreFile,
    _In_ PCWSTR IdArg,
    _In_ PCWSTR ElementName)
{
    WCHAR Buffer[64];
    HKEY Store;
    PCWSTR Id;
    ULONG Type;
    BOOL Ok;

    if (OpenStore(StoreFile, KEY_READ | KEY_WRITE, &Store) != ERROR_SUCCESS)
    {
        PrintResource(IDS_ERR_OPENSTORE);
        return 1;
    }
    Id = IdForAlias(IdArg, Buffer, ARRAYSIZE(Buffer));
    if (!IsValidGuid(Id))
    {
        RegCloseKey(Store);
        PrintResource(IDS_ERR_BADID);
        return 1;
    }
    if (!TypeForName(ElementName, ScopeForStoreObject(Store, Id), &Type))
    {
        RegCloseKey(Store);
        PrintResource(IDS_ERR_BADPARAM);
        return 1;
    }
    Ok = DeleteElement(Store, Id, Type);
    RegCloseKey(Store);

    PrintResource(Ok ? IDS_OK : IDS_ERR_SETFAIL);
    return Ok ? 0 : 1;
}

/* Pack object ids into a REG_MULTI_SZ in Multi; FALSE on overflow. *Chars gets the total length
 * including the final empty string that terminates the list. */
static
BOOL
BuildIdList(
    _Out_writes_(Cap) PWSTR Multi,
    _In_ SIZE_T Cap,
    _In_reads_(Count) PCWSTR *Ids,
    _In_ int Count,
    _Out_ PSIZE_T Chars)
{
    SIZE_T Off = 0;
    int i;

    for (i = 0; i < Count; i++)
    {
        SIZE_T Len = wcslen(Ids[i]);
        if (Off + Len + 2 > Cap)
            return FALSE;
        memcpy(Multi + Off, Ids[i], (Len + 1) * sizeof(WCHAR));
        Off += Len + 1;
    }
    Multi[Off++] = 0;
    *Chars = Off;
    return TRUE;
}

/* Append Id to the list unless it is already present (case-insensitive); FALSE if the list is
 * full, so a too-long order fails the command instead of being silently truncated. */
static
BOOL
AppendId(
    _Inout_updates_(Cap) PCWSTR *List,
    _Inout_ int *Count,
    _In_ int Cap,
    _In_ PCWSTR Id)
{
    int k;

    for (k = 0; k < *Count; k++)
    {
        if (_wcsicmp(List[k], Id) == 0)
            return TRUE;
    }
    if (*Count >= Cap)
        return FALSE;
    List[(*Count)++] = Id;
    return TRUE;
}

/* /displayorder <id> [<id>...] [/addfirst | /addlast | /remove]: edit {bootmgr} DisplayOrder.
 * With no flag the list is set to exactly the given ids; the flags add to or remove from it. */
static
int
DisplayOrderCommand(
    _In_opt_ PCWSTR StoreFile,
    _In_ int Argc,
    _In_reads_(Argc) WCHAR **Argv,
    _In_ int First)
{
    enum { SET, ADDFIRST, ADDLAST, REMOVE } Mode = SET;
    WCHAR IdBuf[64][48], BmBuf[48], Sub[96], Multi[4096], Existing[4096];
    PCWSTR Given[64], Final[128], BootMgr;
    int NumGiven = 0, NumFinal = 0, i, j;
    HKEY Store;
    SIZE_T Chars;
    DWORD RegType = 0, Len;
    BOOL Ok;

    for (i = First; i < Argc; i++)
    {
        if (_wcsicmp(Argv[i], L"/addfirst") == 0)
            Mode = ADDFIRST;
        else if (_wcsicmp(Argv[i], L"/addlast") == 0)
            Mode = ADDLAST;
        else if (_wcsicmp(Argv[i], L"/remove") == 0)
            Mode = REMOVE;
        else
        {
            WCHAR Tmp[48];
            PCWSTR Id = IdForAlias(Argv[i], Tmp, ARRAYSIZE(Tmp));
            if (NumGiven >= ARRAYSIZE(Given))
            {
                PrintResource(IDS_ERR_BADPARAM);
                return 1;
            }
            if (!IsValidGuid(Id))
            {
                PrintResource(IDS_ERR_BADID);
                return 1;
            }
            StringCchCopyW(IdBuf[NumGiven], ARRAYSIZE(IdBuf[0]), Id);
            Given[NumGiven] = IdBuf[NumGiven];
            NumGiven++;
        }
    }
    if (NumGiven == 0)
    {
        PrintResource(IDS_ERR_BADPARAM);
        return 1;
    }

    if (OpenStore(StoreFile, KEY_READ | KEY_WRITE, &Store) != ERROR_SUCCESS)
    {
        PrintResource(IDS_ERR_OPENSTORE);
        return 1;
    }
    BootMgr = IdForAlias(L"{bootmgr}", BmBuf, ARRAYSIZE(BmBuf));

    /* SET uses only the given ids; ADDFIRST puts them ahead of the carried-over order. Either way
     * the given ids lead and duplicates among them collapse. */
    if (Mode == SET || Mode == ADDFIRST)
    {
        for (i = 0; i < NumGiven; i++)
        {
            if (!AppendId(Final, &NumFinal, ARRAYSIZE(Final), Given[i]))
            {
                RegCloseKey(Store);
                PrintResource(IDS_ERR_SETFAIL);
                return 1;
            }
        }
    }

    if (Mode != SET)
    {
        /* Carry over the current order, dropping any id being re-added or removed. A read that
         * fails for anything other than "no such value" must not be treated as an empty list,
         * or an /addlast or /remove would wipe a menu it simply could not read. */
        LONG Read;

        StringCchPrintfW(Sub, ARRAYSIZE(Sub), L"Objects\\%s\\Elements\\24000001", BootMgr);
        Len = sizeof(Existing) - 2 * sizeof(WCHAR);
        Read = RegGetValueW(Store, Sub, L"Element", RRF_RT_REG_MULTI_SZ, &RegType, Existing, &Len);
        if (Read != ERROR_SUCCESS && Read != ERROR_FILE_NOT_FOUND)
        {
            RegCloseKey(Store);
            PrintResource(IDS_ERR_SETFAIL);
            return 1;
        }
        if (Read == ERROR_SUCCESS)
        {
            PCWSTR p = Existing;

            /* A /store hive may hold a list without the final empty string; terminate what we
             * read (the two reserved WCHARs) so the walk cannot run past the data. */
            Existing[Len / sizeof(WCHAR)] = 0;
            Existing[Len / sizeof(WCHAR) + 1] = 0;
            while (*p)
            {
                BOOL Skip = FALSE;
                for (j = 0; j < NumGiven; j++)
                {
                    if (_wcsicmp(p, Given[j]) == 0)
                    {
                        Skip = TRUE;
                        break;
                    }
                }
                if (!Skip && !AppendId(Final, &NumFinal, ARRAYSIZE(Final), p))
                {
                    RegCloseKey(Store);
                    PrintResource(IDS_ERR_SETFAIL);
                    return 1;
                }
                p += wcslen(p) + 1;
            }
        }
        if (Mode == ADDLAST)
        {
            for (i = 0; i < NumGiven; i++)
            {
                if (!AppendId(Final, &NumFinal, ARRAYSIZE(Final), Given[i]))
                {
                    RegCloseKey(Store);
                    PrintResource(IDS_ERR_SETFAIL);
                    return 1;
                }
            }
        }
    }

    if (!BuildIdList(Multi, ARRAYSIZE(Multi), Final, NumFinal, &Chars))
    {
        RegCloseKey(Store);
        PrintResource(IDS_ERR_SETFAIL);
        return 1;
    }
    Ok = WriteElement(Store, BootMgr, 0x24000001, REG_MULTI_SZ, Multi, (DWORD)(Chars * sizeof(WCHAR)));
    RegCloseKey(Store);

    PrintResource(Ok ? IDS_OK : IDS_ERR_SETFAIL);
    return Ok ? 0 : 1;
}

int
wmain(
    _In_ int argc,
    _In_reads_(argc) WCHAR *argv[])
{
    PCWSTR StoreFile = NULL, Command;
    int i;

    /* Pull out "/store <file>" wherever it appears; it precedes the command in normal use. */
    for (i = 1; i + 1 < argc; i++)
    {
        if (_wcsicmp(argv[i], L"/store") == 0)
        {
            StoreFile = argv[i + 1];
            break;
        }
    }

    /* The command is the first argument that is not the /store pair. */
    for (i = 1; i < argc; i++)
    {
        if (_wcsicmp(argv[i], L"/store") == 0)
        {
            i++;            /* skip its value too */
            continue;
        }
        break;
    }

    if (i >= argc)
        return EnumStore(StoreFile) ? 0 : 1;   /* a bare "bcdedit" (or only /store) lists the store */

    Command = argv[i];

    if (_wcsicmp(Command, L"/?") == 0 || _wcsicmp(Command, L"-?") == 0 ||
        _wcsicmp(Command, L"/help") == 0)
    {
        PrintResource(IDS_USAGE);
        return 0;
    }

    if (_wcsicmp(Command, L"/enum") == 0)
    {
        /* The only filter understood so far is "all"; /enum lists every object either way.
         * A narrower filter (ACTIVE, a type, an id) is a later change, so reject the rest. */
        if (i + 1 < argc && argv[i + 1][0] != L'/' && _wcsicmp(argv[i + 1], L"all") != 0)
        {
            PrintResource(IDS_ERR_BADPARAM);
            return 1;
        }
        return EnumStore(StoreFile) ? 0 : 1;
    }

    if (_wcsicmp(Command, L"/set") == 0)
    {
        /* /set <id> <element> <value> [value...]; the element name is resolved against the
         * object's class inside EditElement, since one name can belong to several classes. */
        if (i + 3 >= argc)
        {
            PrintResource(IDS_ERR_BADPARAM);
            return 1;
        }
        return EditElement(StoreFile, argv[i + 1], argv[i + 2], 0, argc, argv, i + 3);
    }

    if (_wcsicmp(Command, L"/deletevalue") == 0)
    {
        /* /deletevalue <id> <element> */
        if (i + 2 >= argc)
        {
            PrintResource(IDS_ERR_BADPARAM);
            return 1;
        }
        return DeleteValueCommand(StoreFile, argv[i + 1], argv[i + 2]);
    }

    if (_wcsicmp(Command, L"/timeout") == 0)
    {
        /* /timeout <seconds> -> {bootmgr} BcdBootMgrInteger_Timeout */
        if (i + 1 >= argc)
        {
            PrintResource(IDS_ERR_BADPARAM);
            return 1;
        }
        return EditElement(StoreFile, L"{bootmgr}", NULL, 0x25000004, argc, argv, i + 1);
    }

    if (_wcsicmp(Command, L"/default") == 0)
    {
        /* /default <id> -> {bootmgr} BcdBootMgrObject_DefaultObject */
        if (i + 1 >= argc)
        {
            PrintResource(IDS_ERR_BADPARAM);
            return 1;
        }
        return EditElement(StoreFile, L"{bootmgr}", NULL, 0x23000003, argc, argv, i + 1);
    }

    if (_wcsicmp(Command, L"/displayorder") == 0)
    {
        /* /displayorder <id> [<id>...] [/addfirst | /addlast | /remove] */
        if (i + 1 >= argc)
        {
            PrintResource(IDS_ERR_BADPARAM);
            return 1;
        }
        return DisplayOrderCommand(StoreFile, argc, argv, i + 1);
    }

    PrintResource(IDS_ERR_BADPARAM);
    return 1;
}
