/*
 * Apple System Management Control (SMC) Tool 
 * Copyright (C) 2006 devnull 
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <IOKit/IOKitLib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

#include "smc.h"

static io_connect_t conn;

UInt32 _strtoul(const char* str, int size, int base)
{
    UInt32 total = 0;
    int i;

    for (i = 0; i < size; i++) {
        if (base == 16)
            total += str[i] << (size - 1 - i) * 8;
        else
            total += (unsigned char)(str[i] << (size - 1 - i) * 8);
    }
    return total;
}

void _ultostr(char* str, UInt32 val)
{
    str[0] = '\0';
    sprintf(str, "%c%c%c%c",
        (unsigned int)val >> 24,
        (unsigned int)val >> 16,
        (unsigned int)val >> 8,
        (unsigned int)val);
}

kern_return_t SMCOpen(void)
{
    kern_return_t result;
    io_iterator_t iterator;
    io_object_t device;

    CFMutableDictionaryRef matchingDictionary = IOServiceMatching("AppleSMC");
    result = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDictionary, &iterator);
    if (result != kIOReturnSuccess) {
        printf("Error: IOServiceGetMatchingServices() = %08x\n", result);
        return 1;
    }

    device = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (device == 0) {
        printf("Error: no SMC found\n");
        return 1;
    }

    result = IOServiceOpen(device, mach_task_self(), 0, &conn);
    IOObjectRelease(device);
    if (result != kIOReturnSuccess) {
        printf("Error: IOServiceOpen() = %08x\n", result);
        return 1;
    }

    return kIOReturnSuccess;
}

kern_return_t SMCClose()
{
    return IOServiceClose(conn);
}

kern_return_t SMCCall(int index, SMCKeyData_t* inputStructure, SMCKeyData_t* outputStructure)
{
    size_t structureInputSize;
    size_t structureOutputSize;

    structureInputSize = sizeof(SMCKeyData_t);
    structureOutputSize = sizeof(SMCKeyData_t);

#if MAC_OS_X_VERSION_10_5
    return IOConnectCallStructMethod(conn, index,
        // inputStructure
        inputStructure, structureInputSize,
        // ouputStructure
        outputStructure, &structureOutputSize);
#else
    return IOConnectMethodStructureIStructureO(conn, index,
        structureInputSize, /* structureInputSize */
        &structureOutputSize, /* structureOutputSize */
        inputStructure, /* inputStructure */
        outputStructure); /* ouputStructure */
#endif
}

kern_return_t SMCReadKey(UInt32Char_t key, SMCVal_t* val)
{
    kern_return_t result;
    SMCKeyData_t inputStructure;
    SMCKeyData_t outputStructure;

    memset(&inputStructure, 0, sizeof(SMCKeyData_t));
    memset(&outputStructure, 0, sizeof(SMCKeyData_t));
    memset(val, 0, sizeof(SMCVal_t));

    inputStructure.key = _strtoul(key, 4, 16);
    inputStructure.data8 = SMC_CMD_READ_KEYINFO;

    result = SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure);
    if (result != kIOReturnSuccess)
        return result;

    val->dataSize = outputStructure.keyInfo.dataSize;
    _ultostr(val->dataType, outputStructure.keyInfo.dataType);
    inputStructure.keyInfo.dataSize = val->dataSize;
    inputStructure.data8 = SMC_CMD_READ_BYTES;

    result = SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure);
    if (result != kIOReturnSuccess)
        return result;

    memcpy(val->bytes, outputStructure.bytes, sizeof(outputStructure.bytes));

    return kIOReturnSuccess;
}

double SMCGetTemperature(char* key)
{
    SMCVal_t val;
    kern_return_t result;

    result = SMCReadKey(key, &val);
    if (result == kIOReturnSuccess) {
        // read succeeded - check returned value
        if (val.dataSize > 0) {
            if (strcmp(val.dataType, DATATYPE_SP78) == 0) {
                // convert sp78 value to temperature
                int intValue = val.bytes[0] * 256 + (unsigned char)val.bytes[1];
                return intValue / 256.0;
            }
        }
    }
    // read failed
    return 0.0;
}

int getTemperatureSMCKeySize(unsigned long core)
{
    return snprintf(NULL, 0, "%s%lu%c", SMC_CPU_CORE_TEMP_PREFIX, core, SMC_CPU_CORE_TEMP_SUFFIX_NEW);
}

void getOldSMCTemperatureKeyTemplate(char* key)
{
    sprintf(key, "%s%%lu%c", SMC_CPU_CORE_TEMP_PREFIX, SMC_CPU_CORE_TEMP_SUFFIX_OLD);
}

void getNewSMCTemperatureKeyTemplate(char* key)
{
    sprintf(key, "%s%%lu%c", SMC_CPU_CORE_TEMP_PREFIX, SMC_CPU_CORE_TEMP_SUFFIX_NEW);
}

double getTemperatureKeyTemplate(unsigned long core, char* templateKey)
{
    getNewSMCTemperatureKeyTemplate(templateKey);

    char key[getTemperatureSMCKeySize(core)];
    sprintf(key, templateKey, core);

    double temperature = SMCGetTemperature(key);

    if (temperature == 0) {
        // We must use the old key
        getOldSMCTemperatureKeyTemplate(templateKey);
        sprintf(key, templateKey, core);
        temperature = SMCGetTemperature(key);
    }

    return temperature;
}

double convertToFahrenheit(double celsius)
{
    return (celsius * (9.0 / 5.0)) + 32.0;
}

unsigned long parseNumArg(char* arg, const char* errorMsg)
{
    char* endptr;
    long result = strtol(arg, &endptr, 10);
    if (endptr == optarg || *endptr != '\0' || result < 0) {
        fprintf(stderr, "%s", errorMsg);
        exit(1);
    }
    return result;
}

unsigned long getCoreArgCount(const char* arg)
{
    unsigned long coreCount = 0;
    for (int i = 0; arg[i] != '\0'; (arg[i] == ',' && !(i == 0 || arg[i + 1] == ',' || arg[i + 1] == '\0')) ? coreCount++ : 0, i++);
    return coreCount + 1;
}

int getPhysicalCoreCount()
{
    // https://stackoverflow.com/a/150971
    int mib[4];
    int numCPU;
    size_t len = sizeof(numCPU);

    /* set the mib for hw.ncpu */
    mib[0] = CTL_HW;
    mib[1] = HW_NCPU; // alternatively, try HW_NCPU;

    /* get the number of CPUs from the system */
    sysctl(mib, 2, &numCPU, &len, NULL, 0);

    if (numCPU < 1) {
        mib[1] = HW_NCPU;
        sysctl(mib, 2, &numCPU, &len, NULL, 0);
        if (numCPU < 1)
            numCPU = 1;
    }

    // https://stackoverflow.com/a/29761237
    uint32_t registers[4];
    __asm__ __volatile__("cpuid "
                         : "=a"(registers[0]),
                         "=b"(registers[1]),
                         "=c"(registers[2]),
                         "=d"(registers[3])
                         : "a"(1), "c"(0));

    unsigned cpuFeatureSet = registers[3];
    bool hyperthreading = cpuFeatureSet & (1 << 28);

    if (hyperthreading)
        return numCPU / 2;
    else
        return numCPU;
}

void getCoreNumbers(char* arg, unsigned long* cores, char* errorMsg)
{
    char buf[strlen(arg) + 1];
    char* core = buf;

    while (*arg) {
        if (!isspace((unsigned char)*arg))
            *core++ = *arg;
        arg++;
    }

    *core = '\0';

    int arrayIndex = 0;
    core = strtok(buf, ",");
    while (core) {
        cores[arrayIndex++] = parseNumArg(core, errorMsg);
        core = strtok(NULL, ",");
    }
}

double convertToCorrectScale(char scale, double temperature) {
    if (scale == 'F') {
        return convertToFahrenheit(temperature);
    } else {
        return temperature;
    }
}

void printTemperature(double temperature, unsigned int rounding) {
    printf("%.*f\n", rounding, temperature);
}

enum OutputMode {
    core,
    package,
};

int main(int argc, char* argv[])
{
    char scale = 'C';
    bool specifiedCores = false;
    unsigned long* coreList = malloc(sizeof(unsigned long));
    unsigned long coreCount;
    unsigned int rounding = 0;
    enum OutputMode outputMode = core;

    int argLabel;
    while ((argLabel = getopt(argc, argv, "FCc:r:ph")) != -1) {
        switch (argLabel) { // NOLINT(hicpp-multiway-paths-covered)
        case 'F':
        case 'C':
            scale = (char)argLabel;
            break;
        case 'c': {
            coreCount = getCoreArgCount(optarg);
            coreList = realloc(coreList, coreCount * sizeof(unsigned long));
            getCoreNumbers(optarg, coreList, "Invalid core specified.\n");
            specifiedCores = true;
            break;
        }
        case 'r':
            rounding = (int)parseNumArg(optarg, "Invalid decimal place limit.\n");
            break;
        case 'p':
            outputMode = package;
            break;
        case 'h':
        case '?':
            printf("usage: coretemp <options>\n");
            printf("Options:\n");
            printf("  -F        Display temperatures in degrees Fahrenheit.\n");
            printf("  -C        Display temperatures in degrees Celsius (Default).\n");
            printf("  -c <num>  Specify which cores to report on, in a comma-separated list. If unspecified, reports all temperatures.\n");
            printf("  -r <num>  The accuracy of the temperature, in the number of decimal places. Defaults to 0.\n");
            printf("  -p        Display the CPU package temperature instead of the core temperatures.\n");
            printf("  -h        Display this help.\n");
            return -1;
        }
    }

    SMCOpen();

    switch (outputMode) {
        default:
        case core: {
            if (!specifiedCores) {
                coreCount = getPhysicalCoreCount();
                coreList = realloc(coreList, coreCount * sizeof(unsigned long));
                for (int i = 0; i < coreCount; ++i)
                    coreList[i] = i;
            }

            char templateKey[7];
            double firstCoreTemperature = getTemperatureKeyTemplate(coreList[0], templateKey);

            if (firstCoreTemperature == 0) {
                // https://logi.wiki/index.php/SMC_Sensor_Codes
                // try again with macbookpro first core temperature = TC1C code(key)
                for (int i = 0; i < coreCount; ++i)
                    coreList[i] = i+1;
                firstCoreTemperature = getTemperatureKeyTemplate(coreList[0], templateKey);
            }

            if (firstCoreTemperature == 0) {
                // The first core does not exist
                printf("The specified core (%lu) does not exist.\n", coreList[0]);
                exit(1);
            }

            printTemperature(convertToCorrectScale(scale, firstCoreTemperature), rounding);

            for (int i = 1; i < coreCount; ++i) {
                char key[getTemperatureSMCKeySize(coreList[i])];
                sprintf(key, templateKey, coreList[i]);
                double temperature = SMCGetTemperature(key);

                if (temperature == 0) {
                    // The specified core does not exist
                    printf("The specified core (%lu) does not exist.\n", coreList[i]);
                    exit(1);
                }

                printTemperature(convertToCorrectScale(scale, temperature), rounding);
            }
            break;
        }
        case package: {
                // https://logi.wiki/index.php/SMC_Sensor_Codes
                // try again with macbookpro cpu proximity temperature = TC0P code(key)
                double cpuTemperature = SMCGetTemperature(SMC_CPU_DIE_TEMP);
                if (cpuTemperature == 0) {
                    cpuTemperature = SMCGetTemperature(SMC_CPU_PPROXIMITY_TEMP);
                }
                printTemperature(convertToCorrectScale(scale, cpuTemperature), rounding);
            }
            break;
    }

    SMCClose();
    return 0;
}
