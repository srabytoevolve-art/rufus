/*
 * Rufus for Linux: Windows User Experience (unattend.xml) generation
 * Copyright © 2022-2026 Pete Batard <pete@akeo.ie>
 * Linux port © 2026 Rufus for Linux contributors
 *
 * Produces the same answer file content as Windows Rufus' CreateUnattendXml().
 * On Linux, it is written as /autounattend.xml at the root of the media,
 * which Windows Setup picks up from removable drives.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "internal.h"

static const char* bypass_name[] = { "BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck" };
static const char* unallowed_account_names[] = {
	"Administrator", "Järjestelmänvalvoja", "Administrateur", "Rendszergazda", "Administrador", "Администратор",
	"Administratör", "Guest", "DefaultAccount", "WDAGUtilityAccount", "HelpAssistant", "KRBTGT", "Local", "NONE", "SYSTEM"
};
#define USERNAME_INVALID_CHARS "/\\[]:;|=,+*?<>\"%@&"

typedef struct {
	char* s[64];
	int n;
} cmdlist_t;

static void add(cmdlist_t* l, const char* s)
{
	if (l->n < (int)ARRAYSIZE(l->s))
		l->s[l->n++] = strdup(s);
}

static void clear(cmdlist_t* l)
{
	for (int i = 0; i < l->n; i++)
		free(l->s[i]);
	l->n = 0;
}

static const char* component_hdr =
	"    <component name=\"%s\" processorArchitecture=\"%s\" language=\"neutral\" "
	"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
	"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n";

char* rf_create_unattend_xml(int flags, const char* arch, const char* username, const char* locale,
	const char* timezone)
{
	char* xml = NULL;
	size_t xml_len = 0;
	char user[64] = "", tmp[256];
	cmdlist_t commands = { 0 };
	FILE* fd;

	if (flags == 0) {
		uprintf("Note: No Windows User Experience options selected");
		return NULL;
	}
	fd = open_memstream(&xml, &xml_len);
	if (fd == NULL)
		return NULL;

	uprintf("Selected Windows User Experience options:");
	fprintf(fd, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
	fprintf(fd, "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n");

	if (flags & UNATTEND_SECUREBOOT_TPM_MINRAM) {
		fprintf(fd, "  <settings pass=\"windowsPE\">\n");
		fprintf(fd, component_hdr, "Microsoft-Windows-Setup", arch);
		/* WinPE will complain if we don't provide a product key. *Any* product key. */
		fprintf(fd, "      <UserData>\n");
		fprintf(fd, "        <AcceptEula>true</AcceptEula>\n");
		fprintf(fd, "        <ProductKey>\n");
		fprintf(fd, "          <Key />\n");
		fprintf(fd, "        </ProductKey>\n");
		fprintf(fd, "      </UserData>\n");
		uprintf("• Bypass SB/TPM/RAM");
		fprintf(fd, "      <RunSynchronous>\n");
		for (size_t i = 0; i < ARRAYSIZE(bypass_name); i++) {
			fprintf(fd, "        <RunSynchronousCommand wcm:action=\"add\">\n");
			fprintf(fd, "          <Order>%d</Order>\n", (int)i + 1);
			fprintf(fd, "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v %s /t REG_DWORD /d 1 /f</Path>\n",
				bypass_name[i]);
			fprintf(fd, "        </RunSynchronousCommand>\n");
		}
		fprintf(fd, "      </RunSynchronous>\n");
		fprintf(fd, "    </component>\n");
		fprintf(fd, "  </settings>\n");
	}

	if (flags & (UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_QOL_ENHANCEMENTS)) {
		fprintf(fd, "  <settings pass=\"specialize\">\n");
		fprintf(fd, component_hdr, "Microsoft-Windows-Deployment", arch);
		if (flags & UNATTEND_NO_ONLINE_ACCOUNT) {
			add(&commands, "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\OOBE\" /v BypassNRO /t REG_DWORD /d 1 /f");
			uprintf("• Bypass online account requirement");
		}
		if (flags & UNATTEND_QOL_ENHANCEMENTS) {
			uprintf("• QoL: Disable OneDrive and Outlook by default");
			add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\OneDrive\" /v DisableFileSyncNGSC /t REG_DWORD /d 1 /f");
			add(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Remove-Item -Path $env:SystemRoot\\System32\\OneDriveSetup.exe -Force -Confirm:$false; "
				"Remove-Item -Path $env:SystemRoot\\SysWOW64\\OneDriveSetup.exe -Force -Confirm:$false;\"");
			add(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Outlook*'} |"
				" Remove-AppxProvisionedPackage -Online\"");
			add(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxPackage -AllUsers *Outlook* | Remove-AppxPackage -AllUsers\"");
			add(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Teams*'} |"
				" Remove-AppxProvisionedPackage -Online\"");
			add(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxPackage -AllUsers *Teams* | Remove-AppxPackage -AllUsers\"");
		}
		for (int order = 1; order <= commands.n; order++) {
			if (order == 1)
				fprintf(fd, "      <RunSynchronous>\n");
			fprintf(fd, "        <RunSynchronousCommand wcm:action=\"add\">\n");
			fprintf(fd, "          <Order>%d</Order>\n", order);
			fprintf(fd, "          <Path>%s</Path>\n", commands.s[order - 1]);
			fprintf(fd, "        </RunSynchronousCommand>\n");
			if (order == commands.n)
				fprintf(fd, "      </RunSynchronous>\n");
		}
		fprintf(fd, "    </component>\n");
		fprintf(fd, "  </settings>\n");
		clear(&commands);
	}

	if (flags & (UNATTEND_NO_DATA_COLLECTION | UNATTEND_SET_USER | UNATTEND_DUPLICATE_LOCALE |
		UNATTEND_DISABLE_BITLOCKER | UNATTEND_QOL_ENHANCEMENTS)) {
		fprintf(fd, "  <settings pass=\"oobeSystem\">\n");
		if (flags & (UNATTEND_NO_DATA_COLLECTION | UNATTEND_SET_USER | UNATTEND_DUPLICATE_LOCALE | UNATTEND_QOL_ENHANCEMENTS)) {
			fprintf(fd, component_hdr, "Microsoft-Windows-Shell-Setup", arch);
			if (flags & UNATTEND_NO_DATA_COLLECTION) {
				uprintf("• Disable data collection");
				fprintf(fd, "      <OOBE>\n");
				fprintf(fd, "        <HideEULAPage>true</HideEULAPage>\n");
				fprintf(fd, "        <ProtectYourPC>3</ProtectYourPC>\n");
				fprintf(fd, "      </OOBE>\n");
			}
			if ((flags & UNATTEND_DUPLICATE_LOCALE) && timezone != NULL && timezone[0] != 0) {
				uprintf("• Use timezone '%s'", timezone);
				fprintf(fd, "      <TimeZone>%s</TimeZone>\n", timezone);
			}
			if ((flags & UNATTEND_SET_USER) && username != NULL) {
				size_t i;
				const char* s = username;
				while (isspace((unsigned char)*s))
					s++;
				snprintf(user, sizeof(user), "%s", s);
				for (i = strlen(user); i > 0 && isspace((unsigned char)user[i - 1]); i--)
					user[i - 1] = 0;
				for (i = 0; i < ARRAYSIZE(unallowed_account_names) && !rf_str_iequal(user, unallowed_account_names[i]); i++);
				if (i < ARRAYSIZE(unallowed_account_names)) {
					uprintf("WARNING: '%s' is not allowed as local account name - Option ignored", user);
				} else if (user[0] != 0) {
					for (char* p = user; *p; p++)
						if (strchr(USERNAME_INVALID_CHARS, *p) != NULL || (unsigned char)*p < 0x20)
							*p = '_';
					uprintf("• Use '%s' for local account name", user);
					fprintf(fd, "      <UserAccounts>\n");
					fprintf(fd, "        <LocalAccounts>\n");
					fprintf(fd, "          <LocalAccount wcm:action=\"add\">\n");
					fprintf(fd, "            <Name>%s</Name>\n", user);
					fprintf(fd, "            <DisplayName>%s</DisplayName>\n", user);
					fprintf(fd, "            <Group>Administrators;Power Users</Group>\n");
					/* Empty password (which needs to be the Base64 encoded UTF-16 string "Password") */
					fprintf(fd, "            <Password>\n");
					fprintf(fd, "              <Value>UABhAHMAcwB3AG8AcgBkAA==</Value>\n");
					fprintf(fd, "              <PlainText>false</PlainText>\n");
					fprintf(fd, "            </Password>\n");
					fprintf(fd, "          </LocalAccount>\n");
					fprintf(fd, "        </LocalAccounts>\n");
					fprintf(fd, "      </UserAccounts>\n");
					/* Since we set a blank password, ask the user to change it at next logon */
					snprintf(tmp, sizeof(tmp), "net user \"%s\" /logonpasswordchg:yes", user);
					add(&commands, tmp);
					add(&commands, "net accounts /maxpwage:unlimited");
				}
			}
			if (flags & UNATTEND_QOL_ENHANCEMENTS) {
				uprintf("• QoL: Disable Fast Startup, Copilot, Recommendations, News and Teams by default");
				add(&commands, "reg add \"HKLM\\System\\CurrentControlSet\\Control\\Session Manager\\Power\" "
					"/v HiberbootEnabled /t REG_DWORD /d 0 /f");
				add(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" "
					"/v ShowCopilotButton /t REG_DWORD /d 0 /f");
				add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\WindowsCopilot\" "
					"/v TurnOffWindowsCopilot /t REG_DWORD /d 1 /f");
				add(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v SearchboxTaskbarMode /t REG_DWORD /d 1 /f");
				add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" "
					"/v DisableWindowsConsumerFeatures /t REG_DWORD /d 1 /f");
				add(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager\" "
					"/v SystemPaneSuggestionsEnabled /t REG_DWORD /d 0 /f");
				add(&commands, "reg add \"HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v BingSearchEnabled /t REG_DWORD /d 0 /f");
				add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Dsh\" "
					"/v AllowNewsAndInterests /t REG_DWORD /d 0 /f");
				add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Windows Feeds\" "
					"/v EnableFeeds /t REG_DWORD /d 0 /f");
				add(&commands, "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Communications\" "
					"/v ConfigureChatAutoInstall /t REG_DWORD /d 0 /f");
				add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Edge\" "
					"/v HideFirstRunExperience /t REG_DWORD /d 1 /f");
				uprintf("• QoL: Restore classic context menu");
				add(&commands, "reg add \"HKCU\\Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32\" "
					"/ve /t REG_SZ /d \"\" /f");
			}
			for (int order = 1; order <= commands.n; order++) {
				if (order == 1)
					fprintf(fd, "      <FirstLogonCommands>\n");
				fprintf(fd, "        <SynchronousCommand wcm:action=\"add\">\n");
				fprintf(fd, "          <Order>%d</Order>\n", order);
				fprintf(fd, "          <CommandLine>%s</CommandLine>\n", commands.s[order - 1]);
				fprintf(fd, "        </SynchronousCommand>\n");
				if (order == commands.n)
					fprintf(fd, "      </FirstLogonCommands>\n");
			}
			fprintf(fd, "    </component>\n");
			clear(&commands);
		}
		if ((flags & UNATTEND_DUPLICATE_LOCALE) && locale != NULL && locale[0] != 0) {
			uprintf("• Use regional settings '%s'", locale);
			fprintf(fd, component_hdr, "Microsoft-Windows-International-Core", arch);
			fprintf(fd, "      <InputLocale>%s</InputLocale>\n", locale);
			fprintf(fd, "      <SystemLocale>%s</SystemLocale>\n", locale);
			fprintf(fd, "      <UserLocale>%s</UserLocale>\n", locale);
			fprintf(fd, "      <UILanguage>%s</UILanguage>\n", locale);
			fprintf(fd, "    </component>\n");
		}
		if (flags & UNATTEND_DISABLE_BITLOCKER) {
			uprintf("• Disable BitLocker");
			fprintf(fd, component_hdr, "Microsoft-Windows-SecureStartup-FilterDriver", arch);
			fprintf(fd, "      <PreventDeviceEncryption>true</PreventDeviceEncryption>\n");
			fprintf(fd, "    </component>\n");
			fprintf(fd, component_hdr, "Microsoft-Windows-EnhancedStorage-Adm", arch);
			fprintf(fd, "      <TCGSecurityActivationDisabled>1</TCGSecurityActivationDisabled>\n");
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_OFFLINE_INTERNAL_DRIVES) {
		uprintf("• Set internal drives offline");
		fprintf(fd, "  <settings pass=\"offlineServicing\">\n");
		fprintf(fd, component_hdr, "Microsoft-Windows-PartitionManager", arch);
		fprintf(fd, "      <SanPolicy>4</SanPolicy>\n");
		fprintf(fd, "    </component>\n");
		fprintf(fd, "  </settings>\n");
	}
	fprintf(fd, "</unattend>\n");
	fclose(fd);
	return xml;
}
