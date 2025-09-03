/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "htOS.h"
#include "htmem.h"  
#include "main.h"
#include "adc.h"
#include "iwdg.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"
#include "httypes.h" 
#include "tim2_utils.h" 

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "bsp_usart.h"
#include "bsp_lora.h"	
#include "bsp_keyscan.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */


#define TASK_STACK_SIZE 512 

#define COUNTOF(__BUFFER__)   (sizeof(__BUFFER__) / sizeof(*(__BUFFER__)))
#define TXBUFFERSIZE1         (COUNTOF(aTxBuffer1) - 1)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void Power_On_Selftest(void);
void Tempture_Test(void);

/* USER CODE BEGIN PFP */

/* 任务句柄 */
TaskHandle_t Task1Handle;
TaskHandle_t Task2Handle;
TaskHandle_t Task3Handle;
/* 定义队列和信号量句柄 */
QueueHandle_t xQueue;
SemaphoreHandle_t xSemaphore;
//SemaphoreHandle_t xMutex;

/* 定义数据结构 */
typedef struct
{
    uint32_t id;
    uint32_t value;
		uint32_t paper;
} DataItem_t;

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static void task1Function(void* param) {
    DataItem_t receivedItem;
    for (;;)
    {
        /* 等待生产者的通知 */
        if (htOSSemaphoreTake(xSemaphore, 200) == 0)
        {
            
            /* 从队列接收数据 */
            if (htOSQueueReceive(xQueue, &receivedItem, 200) == 0)
            {
                printf("Consumer: Received ID=%d, Value=%d \r\n", 
                       receivedItem.id, receivedItem.value);
            }
            else
            {
                printf("xQueue receive error\r\n");
            }
            
        }
        else
        {
           printf("xSemaphore take error\r\n");
        }
    }
}

static void task2Function(void* param) {
    uint32_t counter = 0;
    DataItem_t item;
    for (;;)
    {
        /* 准备数据 */
        item.id 	 = ++counter;
        item.value = counter * 2;
        if (htOSQueueSend(xQueue, &item, 2000) == 0)
        {        
            /* 释放信号量通知消费者 */
            htOSSemaphoreGive(xSemaphore);
        }
        else
        {
           printf("xQueue send error\r\n");
        }
        
        /* 延时一段时间 */
        htTaskDelay(1000);
    }
}

static void task3Function(void* param) {
    uint32_t counter = 0;
    DataItem_t item;
    for (;;)
    {
        /* 准备数据 */
        item.id 	 = ++counter;
        item.value = counter * 20;
        if (htOSQueueSend(xQueue, &item, 2000) == 0)
        {  
            /* 释放信号量通知消费者 */
            htOSSemaphoreGive(xSemaphore);				
        }
        else
        {
            printf("xQueue send error\r\n");
        }
        
        /* 延时一段时间 */
        htTaskDelay(1000);
    }
}



static void task4Function(void* param) {
    int counter = 0;
    for(;;)
		{
        counter++;
        printf("Task4 running (%d)\r\n", counter);
        htTaskDelay(1000); //		
		}

}



/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
    /* USER CODE BEGIN 1 */

    /* USER CODE END 1 */

    /* MCU Configuration--------------------------------------------------------*/

    /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
    HAL_Init();

    /* USER CODE BEGIN Init */

    /* USER CODE END Init */

    /* Configure the system clock */
    SystemClock_Config();

    /* USER CODE BEGIN SysInit */

    /* USER CODE END SysInit */

    /* Initialize all configured peripherals */
    MX_GPIO_Init();
    //MX_ADC1_Init();
    MX_SPI1_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();
    MX_USART3_UART_Init();
    
    // 先初始化TIM2，使其可用于HAL_TIM2_Delay
    MX_TIM2_Init();
    HAL_TIM_Base_Start_IT(&htim2); //定时器使能中断   
    //MX_IWDG_Init();
    //__HAL_IWDG_START(&hiwdg);//启动看门狗
    Power_On_Selftest();
    RS485_Send;//485收
    Usart_Interpt_on();//启动串口中断接收
    HAL_ADC_Start_IT(&hadc1); //启动ADC中断
    LoRa_Init(LoRaNet);
    U3PassiveEvent((uint8_t *)aRxBuffer1, RX_len1);
    /* 创建队列 */
    xQueue = htOSQueueCreate(5, sizeof(DataItem_t));
    /* 创建信号量 */
    xSemaphore = htOSSemaphoreCreateBinary();
		
    /* USER CODE END 2 */


    
    printf("准备初始化htOS...\r\n");
    HAL_TIM2_Delay(5); // 确保printf输出完成
    int ret = htOSInit();
    if (ret != 0) 
		{
        printf("错误: htOS初始化失败\r\n");
				while(1) 
				{
            HAL_TIM2_Delay(100);
        }
    }


    Task1Handle = htOSTaskCreate("Task1", task1Function, NULL , TASK_STACK_SIZE, HT_LOW_TASK);
    Task2Handle = htOSTaskCreate("Task2", task2Function, NULL , TASK_STACK_SIZE, HT_LOW_TASK);  
	  Task3Handle = htOSTaskCreate("Task3", task3Function, NULL , TASK_STACK_SIZE, HT_LOW_TASK);  
		Task3Handle = htOSTaskCreate("Task4", task4Function, NULL , TASK_STACK_SIZE, HT_LOW_TASK); 
		//htOSTaskDelete(Task3Handle);
    // 启动操作系统调度器
    htOSStart();

    while(1) 
		{
        HAL_TIM2_Delay(50);
    }
}


/* USER CODE BEGIN 4 */
void Power_On_Selftest(void) {  // 一秒
    Tempture_Test();
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET) {
        printf("BAD\r\n");  // 独立看门狗复位
        __HAL_RCC_CLEAR_RESET_FLAGS();  // 清除看门狗标志位
    } else {
        printf("GOOD\r\n");  // 正常复位
    }
}

void Tempture_Test(void) {
    double adc_val = (uint32_t)(ADC_Vol * 100); 
    if (adc_val > 400)  // 4.0 * 100 = 400
        Mcu_State_ON;
    else
        Mcu_State_OFF;
}
/* USER CODE END 4 */








/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

    /** Initializes the RCC Oscillators according to the specified parameters
    * in the RCC_OscInitTypeDef structure.
    */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.LSIState = RCC_LSI_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL12;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    /** Initializes the CPU, AHB and APB buses clocks
    */
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                                |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
        Error_Handler();
    }
}



/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
    /* USER CODE BEGIN Error_Handler_Debug */
    /* User can add his own implementation to report the HAL error return state */
    __disable_irq();
    while (1)
    {
    }
    /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
    /* USER CODE BEGIN 6 */
    /* User can add his own implementation to report the file name and line number,
       ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
    /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
