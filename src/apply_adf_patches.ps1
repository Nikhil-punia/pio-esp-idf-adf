# Script to manually apply ESP-ADF patches to ESP-IDF v5.2 in PowerShell

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

    # Function code to insert
    $functionCode = @"
    
    /*-----------------------------------------------------------*/

    BaseType_t xTaskCreateRestrictedPinnedToCore( const TaskParameters_t * const pxTaskDefinition, TaskHandle_t *pxCreatedTask, const BaseType_t xCoreID)
    {
        TCB_t *pxNewTCB;
        BaseType_t xReturn = errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;

        configASSERT( pxTaskDefinition->puxStackBuffer );

        if( pxTaskDefinition->puxStackBuffer != NULL )
        {
            /* Allocate space for the TCB. */
            pxNewTCB = ( TCB_t * ) pvPortMalloc( sizeof( TCB_t ) );

            if( pxNewTCB != NULL )
            {
                memset(pxNewTCB, 0, sizeof(TCB_t));
                pxNewTCB->pxStack = pxTaskDefinition->puxStackBuffer;
                pxNewTCB->ucStaticallyAllocated = tskDYNAMICALLY_ALLOCATED_STACK_AND_TCB;

                prvInitialiseNewTask(
                    pxTaskDefinition->pvTaskCode,
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
"@

    # Insert after 'return xReturn;'
    $fileContent = $fileContent -replace "(return xReturn;)", "`$1`n$functionCode"

    # Save file
    Set-Content -Path $FREERTOS_ADDITIONS_FILE -Value $fileContent -Encoding UTF8

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

    # Insert declaration after xTaskCreatePinnedToCore
    $declaration = @"
    BaseType_t xTaskCreateRestrictedPinnedToCore( const TaskParameters_t * const pxTaskDefinition,
                                                  TaskHandle_t *pxCreatedTask,
                                                  const BaseType_t xCoreID);
"@

    $fileContent = $fileContent -replace "(BaseType_t xTaskCreatePinnedToCore.*;)", "`$1`n$declaration"

    # Save file
    Set-Content -Path $IDF_ADDITIONS_FILE -Value $fileContent -Encoding UTF8

    Write-Host "Function declaration added to idf_additions.h"
}
else {
    Write-Host "xTaskCreateRestrictedPinnedToCore declaration already exists in idf_additions.h"
}

Write-Host "ESP-ADF patches applied successfully!"
Write-Host "You can now rebuild your project with PSRAM task stack support."
