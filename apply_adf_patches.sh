#!/bin/bash

# Script to manually apply ESP-ADF patches to ESP-IDF v5.2

ESP_IDF_PATH="/home/codespace/.platformio/packages/framework-espidf"

echo "Applying ESP-ADF patches to ESP-IDF v5.2..."

# 1. Add xTaskCreateRestrictedPinnedToCore function to freertos_tasks_c_additions.h
FREERTOS_ADDITIONS_FILE="$ESP_IDF_PATH/components/freertos/esp_additions/freertos_tasks_c_additions.h"

# Check if the function already exists
if ! grep -q "xTaskCreateRestrictedPinnedToCore" "$FREERTOS_ADDITIONS_FILE"; then
    echo "Adding xTaskCreateRestrictedPinnedToCore function to freertos_tasks_c_additions.h..."
    
    # Create a backup
    cp "$FREERTOS_ADDITIONS_FILE" "$FREERTOS_ADDITIONS_FILE.backup"
    
    # Find the line with "return xReturn;" and add the new function after it
    sed -i '/return xReturn;/a\\n\n    /*-----------------------------------------------------------*/\n\n    BaseType_t xTaskCreateRestrictedPinnedToCore( const TaskParameters_t * const pxTaskDefinition, TaskHandle_t *pxCreatedTask, const BaseType_t xCoreID)\n    {\n        TCB_t *pxNewTCB;\n        BaseType_t xReturn = errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;\n\n        configASSERT( pxTaskDefinition->puxStackBuffer );\n\n        if( pxTaskDefinition->puxStackBuffer != NULL )\n        {\n            /* Allocate space for the TCB.  Where the memory comes from depends\n            on the implementation of the port malloc function and whether or\n            not static allocation is being used. */\n            pxNewTCB = ( TCB_t * ) pvPortMalloc( sizeof( TCB_t ) );\n\n            if( pxNewTCB != NULL )\n            {\n                memset(pxNewTCB, 0, sizeof(TCB_t));\n                /* Store the stack location in the TCB. */\n                pxNewTCB->pxStack = pxTaskDefinition->puxStackBuffer;\n\n                /* Tasks can be created statically or dynamically, so note\n                this task had a statically allocated stack in case it is\n                later deleted.  The TCB was allocated dynamically. */\n                pxNewTCB->ucStaticallyAllocated = tskDYNAMICALLY_ALLOCATED_STACK_AND_TCB;\n\n                prvInitialiseNewTask(   pxTaskDefinition->pvTaskCode,\n                                        pxTaskDefinition->pcName,\n                                        pxTaskDefinition->usStackDepth,\n                                        pxTaskDefinition->pvParameters,\n                                        pxTaskDefinition->uxPriority,\n                                        pxCreatedTask, pxNewTCB,\n                                        pxTaskDefinition->xRegions,\n                                        xCoreID );\n\n                prvAddNewTaskToReadyList( pxNewTCB );\n                xReturn = pdPASS;\n            }\n        }\n\n        return xReturn;\n    }' "$FREERTOS_ADDITIONS_FILE"
    
    echo "Function added to freertos_tasks_c_additions.h"
else
    echo "xTaskCreateRestrictedPinnedToCore function already exists in freertos_tasks_c_additions.h"
fi

# 2. Add function declaration to idf_additions.h
IDF_ADDITIONS_FILE="$ESP_IDF_PATH/components/freertos/esp_additions/include/freertos/idf_additions.h"

if ! grep -q "xTaskCreateRestrictedPinnedToCore" "$IDF_ADDITIONS_FILE"; then
    echo "Adding function declaration to idf_additions.h..."
    
    # Create a backup
    cp "$IDF_ADDITIONS_FILE" "$IDF_ADDITIONS_FILE.backup"
    
    # Add the function declaration after the xTaskCreatePinnedToCore declaration
    sed -i '/BaseType_t xTaskCreatePinnedToCore/a\\n    BaseType_t xTaskCreateRestrictedPinnedToCore( const TaskParameters_t * const pxTaskDefinition,\n                                                TaskHandle_t *pxCreatedTask,\n                                                const BaseType_t xCoreID);' "$IDF_ADDITIONS_FILE"
    
    echo "Function declaration added to idf_additions.h"
else
    echo "xTaskCreateRestrictedPinnedToCore declaration already exists in idf_additions.h"
fi

echo "ESP-ADF patches applied successfully!"
echo "You can now rebuild your project with PSRAM task stack support."
