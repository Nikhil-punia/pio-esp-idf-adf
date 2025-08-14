# Script to manually apply ESP-ADF patches to ESP-IDF v5.2 in PowerShell
# Based on official ESP-ADF patch: freertos: add task create API for allocated stack in psram

$ESP_IDF_PATH = "C:\Users\mailn\.platformio\packages\framework-espidf"

Write-Host "Applying ESP-ADF patches"

# 1. Add xTaskCreateRestrictedPinnedToCore function to freertos_tasks_c_additions.h
$FREERTOS_ADDITIONS_FILE = Join-Path $ESP_IDF_PATH "components\freertos\esp_additions\freertos_tasks_c_additions.h"

if (-not (Select-String -Path $FREERTOS_ADDITIONS_FILE -Pattern "xTaskCreateRestrictedPinnedToCore" -Quiet)) {
    Write-Host "Adding xTaskCreateRestrictedPinnedToCore function to freertos_tasks_c_additions.h..."

    # Create a backup
    Copy-Item $FREERTOS_ADDITIONS_FILE "$FREERTOS_ADDITIONS_FILE.backup"

    # Read the file
    $fileContent = Get-Content $FREERTOS_ADDITIONS_FILE -Raw

    # Function code to insert (matching the official patch exactly)
    $functionCode = @"

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )

    BaseType_t xTaskCreateRestrictedPinnedToCore( const TaskParameters_t * const pxTaskDefinition, TaskHandle_t *pxCreatedTask, const BaseType_t xCoreID)
    {
	    TCB_t *pxNewTCB;
	    BaseType_t xReturn = errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;

		configASSERT( pxTaskDefinition->puxStackBuffer );

		if( pxTaskDefinition->puxStackBuffer != NULL )
		{
			/* Allocate space for the TCB.  Where the memory comes from depends
			on the implementation of the port malloc function and whether or
			not static allocation is being used. */
			pxNewTCB = ( TCB_t * ) pvPortMalloc( sizeof( TCB_t ) );

			if( pxNewTCB != NULL )
			{
                memset( pxNewTCB, 0, sizeof( TCB_t ) );
				/* Store the stack location in the TCB. */
				pxNewTCB->pxStack = pxTaskDefinition->puxStackBuffer;

				/* Tasks can be created statically or dynamically, so note
				this task had a statically allocated stack in case it is
				later deleted.  The TCB was allocated dynamically. */
				pxNewTCB->ucStaticallyAllocated = tskDYNAMICALLY_ALLOCATED_STACK_AND_TCB;

				prvInitialiseNewTask(	pxTaskDefinition->pvTaskCode,
										pxTaskDefinition->pcName,
										pxTaskDefinition->usStackDepth,
										pxTaskDefinition->pvParameters,
										pxTaskDefinition->uxPriority,
										pxCreatedTask, pxNewTCB,
										pxTaskDefinition->xRegions,
										xCoreID );

				prvAddNewTaskToReadyList( pxNewTCB );
				xReturn = pdPASS;
			}
		}

		return xReturn;
    }

#endif /* ( configSUPPORT_STATIC_ALLOCATION == 1 ) */
/*----------------------------------------------------------*/
"@

    # Find insertion point after the last configSUPPORT_STATIC_ALLOCATION block
    $pattern = "#endif /\* \( configSUPPORT_STATIC_ALLOCATION == 1 \) \*/`r?`n/\*----------------------------------------------------------\*/"
    if ($fileContent -match $pattern) {
        $fileContent = $fileContent -replace "($pattern)", "$functionCode`r`n`$1"
    } else {
        # Fallback: find after last static allocation endif
        $fallbackPattern = "#endif /\* \( configSUPPORT_STATIC_ALLOCATION == 1 \) \*/"
        if ($fileContent -match $fallbackPattern) {
            $fileContent = $fileContent -replace "($fallbackPattern)", "`$1`r`n$functionCode"
        } else {
            # Last resort: add before last line
            $lines = $fileContent -split "`r?`n"
            $lastLine = $lines[-1]
            $contentWithoutLastLine = $lines[0..($lines.Length-2)] -join "`r`n"
            $fileContent = $contentWithoutLastLine + "`r`n" + $functionCode + "`r`n" + $lastLine
        }
    }

    # Save file
    Set-Content -Path $FREERTOS_ADDITIONS_FILE -Value $fileContent -Encoding UTF8 -NoNewline

    Write-Host "Function added to freertos_tasks_c_additions.h"
}
else {
    Write-Host "xTaskCreateRestrictedPinnedToCore function already exists in freertos_tasks_c_additions.h"
}

# 2. Add function declaration to idf_additions.h
$IDF_ADDITIONS_FILE = Join-Path $ESP_IDF_PATH "components\freertos\esp_additions\include\freertos\idf_additions.h"

if (-not (Select-String -Path $IDF_ADDITIONS_FILE -Pattern "xTaskCreateRestrictedPinnedToCore" -Quiet)) {
    Write-Host "Adding function declaration to idf_additions.h..."

    # Create a backup
    Copy-Item $IDF_ADDITIONS_FILE "$IDF_ADDITIONS_FILE.backup"

    # Read file
    $fileContent = Get-Content $IDF_ADDITIONS_FILE -Raw

    # Function declaration to insert
    $declaration = @"

    BaseType_t xTaskCreateRestrictedPinnedToCore( const TaskParameters_t * const pxTaskDefinition, TaskHandle_t *pxCreatedTask, const BaseType_t xCoreID);
"@

    # Find the position right before "#endif /* configSUPPORT_STATIC_ALLOCATION */"
    $pattern = "#endif /\* configSUPPORT_STATIC_ALLOCATION \*/"
    if ($fileContent -match $pattern) {
        $fileContent = $fileContent -replace "($pattern)", "$declaration`r`n`r`n`$1"
    } else {
        # Fallback: add after xTaskCreateStaticPinnedToCore declaration
        $fallbackPattern = "BaseType_t xTaskCreateStaticPinnedToCore.*?;"
        if ($fileContent -match $fallbackPattern) {
            $fileContent = $fileContent -replace "($fallbackPattern)", "`$1$declaration"
        } else {
            # Last resort: add before the last line
            $lines = $fileContent -split "`r?`n"
            $lastLine = $lines[-1]
            $contentWithoutLastLine = $lines[0..($lines.Length-2)] -join "`r`n"
            $fileContent = $contentWithoutLastLine + $declaration + "`r`n" + $lastLine
        }
    }

    # Save file
    Set-Content -Path $IDF_ADDITIONS_FILE -Value $fileContent -Encoding UTF8 -NoNewline

    Write-Host "Function declaration added to idf_additions.h"
}
else {
    Write-Host "xTaskCreateRestrictedPinnedToCore declaration already exists in idf_additions.h"
}

# 3. Add linker entry to linker_common.lf
$LINKER_FILE = Join-Path $ESP_IDF_PATH "components\freertos\linker_common.lf"

if (-not (Select-String -Path $LINKER_FILE -Pattern "xTaskCreateRestrictedPinnedToCore" -Quiet)) {
    Write-Host "Adding linker entry to linker_common.lf..."

    # Create a backup
    Copy-Item $LINKER_FILE "$LINKER_FILE.backup"

    # Read file
    $fileContent = Get-Content $LINKER_FILE -Raw

    # Find the line with xTaskCreateStaticPinnedToCore and add after it
    $pattern = "tasks:xTaskCreateStaticPinnedToCore \(default\)"
    if ($fileContent -match [regex]::Escape($pattern)) {
        $replacement = $pattern + "`r`n        tasks:xTaskCreateRestrictedPinnedToCore (default)"
        $fileContent = $fileContent -replace [regex]::Escape($pattern), $replacement
        
        # Save file
        Set-Content -Path $LINKER_FILE -Value $fileContent -Encoding UTF8 -NoNewline
        Write-Host "Linker entry added to linker_common.lf"
    } else {
        Write-Host "Warning: Could not find insertion point in linker_common.lf. Please add manually:"
        Write-Host "        tasks:xTaskCreateRestrictedPinnedToCore (default)"
    }
}
else {
    Write-Host "xTaskCreateRestrictedPinnedToCore linker entry already exists in linker_common.lf"
}

Write-Host "ESP-ADF patches applied successfully!"
Write-Host "You can now rebuild your project with PSRAM task stack support."
