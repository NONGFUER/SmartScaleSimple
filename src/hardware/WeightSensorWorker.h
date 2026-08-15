#ifndef WEIGHTSENSORWORKER_H
#define WEIGHTSENSORWORKER_H

#include <QObject>
#include <QTimer>
#include <QSerialPort>
#include <QByteArray>
#include <QThread>
#include <QMutex>
#include <atomic>

/**
 * @brief 串口 I/O 工作线程 — 承载所有阻塞式 Modbus 通信
 *
 * 运行在独立 QThread 中, 通过信号与 GUI 线程的 WeightSensor 通信:
 *   GUI → Worker:  requestTare() / requestCalibrate(cmd)
 *   Worker → GUI:  weightDataReady(int32_t weight_g, uint16_t status, int32_t adc)
 *                 tareDone(bool ok)
 *                 calibrateDone(bool ok)
 *                 errorOccurred(const QString &msg)
 *
 * 协议变体由编译期宏控制:
 *   -DSMARTSCALE_MODBUS_PROTOCOL=feigong (默认) → SMARTSCALE_MODBUS_PROTOCOL_FEIGONG
 *   -DSMARTSCALE_MODBUS_PROTOCOL=v2            → SMARTSCALE_MODBUS_PROTOCOL_V2
 *
 * 串口参数与轮询超时通过环境变量配置 (initSerial() 中读取):
 *   SMARTSCALE_SERIAL_PORT       默认 /dev/ttyAMA0
 *   SMARTSCALE_SERIAL_BAUD       默认 9600
 *   SMARTSCALE_MODBUS_SLAVE      默认 1
 *   SMARTSCALE_POLL_INTERVAL_MS  默认 200
 *   SMARTSCALE_READ_TIMEOUT_MS   默认 1000
 *
 * 状态字在 emit 前统一归一化到 Feigong 位定义:
 *   Bit0=稳定  Bit1=过载  Bit2=负重  Bit3=去皮
 * (V2 路径在 modbusReadWeight() 内部完成位重映射)
 */
class WeightSensorWorker : public QObject
{
    Q_OBJECT

public:
    explicit WeightSensorWorker(QObject *parent = nullptr);
    ~WeightSensorWorker();

    /** 启动轮询定时器 (必须在 Worker 线程中调用) */
    void startPolling();

public Q_SLOTS:
    /** 去皮请求 (来自 GUI 线程的 QueuedConnection) */
    void doTare();
    /** 校准请求 (预留接口, cmd=2 半量程 / cmd=3 满量程, 仅 Feigong 路径有意义) */
    void doCalibrate(uint16_t cmd);
    /** 读取设备序列号 SN (0x0031 起 8 个寄存器 = 16 字节 ASCII) */
    void doReadSN();

Q_SIGNALS:
    /** 称重数据就绪 (跨线程 → GUI) */
    void weightDataReady(int32_t weight_g, uint16_t status, int32_t adc_raw);
    /** 去皮完成 (跨线程 → GUI) */
    void tareDone(bool ok);
    /** 校准完成 (跨线程 → GUI) */
    void calibrateDone(bool ok);
    /** 错误通知 (跨线程 → GUI) */
    void errorOccurred(const QString &msg);
    /** 序列号读取完成 (跨线程 → GUI), 空字符串表示失败 */
    void snReady(const QString &sn);

private Q_SLOTS:
    /** 定时轮询槽 (由内部 QTimer 驱动) */
    void poll();

private:
    // ==================== Modbus RTU 协议 ====================
    static uint16_t crc16Modbus(const uint8_t *data, uint16_t len);
    static int32_t  beToInt32(const uint8_t *p);
    static uint16_t beToUint16(const uint8_t *p);
    static int32_t  leToInt32(const uint8_t *p);
    static float    leToFloat(const uint8_t *p);
    static int16_t  beToInt16(const uint8_t *p);
    /** V2 旧协议状态位 → Feigong 标准状态位 重映射 */
    static uint16_t remapV2StatusToFeigong(uint16_t v2Status);

    // ==================== Modbus 公共辅助 (消除样板) ====================
    /** 构造功能码03读请求帧 (8字节, 含CRC) */
    void buildReadFrame(uint16_t regAddr, uint16_t count, uint8_t tx[8]);
    /** 构造功能码06写请求帧 (8字节, 含CRC) */
    void buildWriteFrame(uint16_t regAddr, uint16_t value, uint8_t tx[8]);
    /** 清空输入并整帧发送 (clear+write+flush), 成功返回 true */
    bool sendFrame(const uint8_t *tx, int len);
    /** 循环读取响应帧直至收满 expectedLen 字节或 m_readTimeoutMs 超时 */
    QByteArray readFrame(int expectedLen);
    /** CRC + 异常响应校验, 返回 0=OK -2=CRC错 -3=异常响应 */
    int verifyResponse(const QByteArray &rx, int frameLen);
    /** 字节数组转空格分隔的大写十六进制串 (日志用) */
    static QString toHexString(const uint8_t *p, int n);

    /** 功能码03: 读取净重+状态+ADC */
    int modbusReadWeight(int32_t *weight_g, uint16_t *status, int32_t *adc_raw);
    /** 功能码03: 读取设备序列号 SN (0x0031 起 8 个寄存器 = 16 字节 ASCII) */
    int modbusReadSN(QString *sn);
    /** 功能码06: 写单个寄存器 (去皮/校准) */
    int modbusWriteCmd(uint16_t regAddr, uint16_t value);

    // ==================== 串口 ====================
    QSerialPort *m_serial;
    bool initSerial();
    /** 从环境变量读取初始参数, 失败时使用默认值 */
    void loadConfigFromEnv();

    // ==================== 互斥与自愈 ====================
    QMutex m_serialMutex;              // 串口操作互斥锁
    std::atomic<bool> m_isBusy{false};  // 命令执行中标志 (poll 跳过), 原子操作防竞态
    int    m_consecutiveErrors = 0;    // 连续错误计数
    void   restartSerial();            // 连续失败后重启串口
    static constexpr int kMaxConsecutiveErrors = 5;  // 触发重启的阈值

    // ==================== 定时器 (Worker 线程内驱动) ====================
    QTimer *m_pollTimer;

    // ==================== 运行时配置 (环境变量注入) ====================
    QString m_portName;          // 串口设备路径
    qint32  m_baudRate;          // 波特率
    uint8_t m_slaveAddr;         // Modbus 从站地址
    int     m_pollIntervalMs;    // 轮询间隔
    int     m_readTimeoutMs;     // 读响应总超时

    // ==================== Modbus 常量 ====================
    // 寄存器/帧长按协议宏切换; 命令寄存器与去皮值两协议一致
#ifdef SMARTSCALE_MODBUS_PROTOCOL_V2
    // V2 旧协议: 读 0x0010 起 9 个寄存器, 响应帧 23 字节
    static constexpr uint16_t REG_DATA_ADDR  = 0x0010;
    static constexpr uint16_t REG_DATA_COUNT = 9;
    static constexpr int      FRAME_LEN      = 23;
#else
    // Feigong 标准协议: 读 0x0000 起 5 个寄存器, 响应帧 15 字节
    static constexpr uint16_t REG_DATA_ADDR  = 0x0000;
    static constexpr uint16_t REG_DATA_COUNT = 5;
    static constexpr int      FRAME_LEN      = 15;
#endif
    static constexpr uint8_t  FUNC_READ      = 0x03;
    static constexpr uint8_t  FUNC_WRITE     = 0x06;
    static constexpr uint16_t REG_CMD_ADDR   = 0x0010;  // System_Cmd 命令寄存器
    static constexpr uint16_t CMD_TARE       = 1;        // 去皮
    static constexpr uint16_t CMD_CALIB_HALF = 2;        // 半量程标定 (Feigong)
    static constexpr uint16_t CMD_CALIB_FULL = 3;        // 满量程标定 (Feigong)

    // 序列号寄存器: 0x0031 起 8 个寄存器 = 16 字节 ASCII
    // 请求帧: 01 03 00 31 00 08 15 C3   (8B)
    // 响应帧: 01 03 10 [16B SN] CRC CRC (21B)
    static constexpr uint16_t REG_SN_ADDR    = 0x0031;
    static constexpr uint16_t REG_SN_COUNT   = 8;
    static constexpr int      SN_FRAME_LEN   = 21;       // 1+1+1+16+2

    // ==================== 超时参数 (默认值, 可被环境变量覆盖) ====================
    static constexpr int DEFAULT_POLL_INTERVAL_MS = 200;
    static constexpr int DEFAULT_READ_TIMEOUT_MS  = 1000;
    static constexpr int SINGLE_WAIT_MS    = 30;    // 单次 waitForReadyRead
};

#endif // WEIGHTSENSORWORKER_H
