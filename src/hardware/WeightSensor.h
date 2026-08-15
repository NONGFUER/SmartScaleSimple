#ifndef WEIGHTSENSOR_H
#define WEIGHTSENSOR_H

#include <QObject>
#include <QThread>
#include <QTimer>

class WeightSensorWorker;

struct WeightSample {
    double weightKg = 0;
    uint16_t statusWord = 0;
    int32_t adcRaw = 0;
};

/**
 * @brief 称重传感器 (GUI 线程薄代理)
 *
 * 职责:
 *   - QML 属性 (netWeight, isStable) + 信号通知
 *   - 滑动窗口滤波 & 稳定检测
 *   - 管理 Worker 线程生命周期
 *
 * 所有阻塞式串口 I/O 已移至 WeightSensorWorker (独立线程)
 */
class WeightSensor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double netWeight READ netWeight NOTIFY weightChanged)
    Q_PROPERTY(double displayWeight READ displayWeight NOTIFY displayWeightChanged)
    Q_PROPERTY(bool isStable READ isStable NOTIFY stableChanged)
    Q_PROPERTY(QString sn READ sn NOTIFY snChanged)

public:
    explicit WeightSensor(QObject *parent = nullptr);
    ~WeightSensor();

    double netWeight() const;
    double displayWeight() const;
    bool isStable() const;
    QString sn() const;

    /** 去皮: 通过信号转发到 Worker 线程 */
    Q_INVOKABLE void tare();
    /** 归零: 等同硬件去皮，但当前净重 > 8kg 时拒绝(硬件限制) */
    Q_INVOKABLE void zero();
    /** 半量程标定 (预留接口, Feigong 协议下有效; 校准前需先去皮空载再放砝码) */
    Q_INVOKABLE void calibrateHalf();
    /** 满量程标定 (预留接口, Feigong 协议下有效) */
    Q_INVOKABLE void calibrateFull();
    /** 读取设备序列号 SN (异步, 结果通过 snChanged 信号通知) */
    Q_INVOKABLE void readSN();

Q_SIGNALS:
    void weightChanged();
    void displayWeightChanged();  // 显示重量变化（带迟滞，跳变才发）
    void stableChanged();
    void stableTriggered(); // 重量从"不稳定"刚变为"稳定的瞬间触发"
    /** 去皮完成通知 (true=成功, false=失败) */
    void tareDone(bool ok);
    /** 校准完成通知 (true=命令已确认, false=失败) */
    void calibrateDone(bool ok);
    /** 序列号变更通知 (读取完成或失败时空字符串) */
    void snChanged();

    /** 内部信号: 向 Worker 线程发送去皮请求 */
    void requestTare();
    /** 内部信号: 向 Worker 线程发送校准请求 (cmd=2 半量程 / cmd=3 满量程) */
    void requestCalibrate(uint16_t cmd);
    /** 内部信号: 向 Worker 线程发送读取 SN 请求 */
    void requestReadSN();

private Q_SLOTS:
    /** 接收 Worker 的称重数据, 写入缓冲池 */
    void onWeightDataReady(int32_t weight_g, uint16_t status, int32_t adc_raw);
    /** 接收 Worker 的去皮结果 */
    void onTareDone(bool ok);
    /** 接收 Worker 的校准结果 */
    void onCalibrateDone(bool ok);
    /** 接收 Worker 的 SN 读取结果 */
    void onSnReady(const QString &sn);
    /** 消费缓冲池数据 (低频定时器驱动) */
    void consumeBuffer();

private:
    // ==================== 线程管理 ====================
    QThread       *m_workerThread;
    WeightSensorWorker *m_worker;

    // ==================== 称重状态 ====================
    double m_netWeight;     // 净重 (kg)
    bool   m_isStable;
    double m_zeroOffset;    // 软件零点偏移 (kg)
    double m_tareWeight;    // 皮重 (kg)

    // ==================== 稳定检测 & 触发防抖 ====================
    static constexpr double MIN_TRIGGER_WEIGHT_KG = 0.15;     // 最小触发重量(150g)，过滤微小数据误报
    static constexpr int    MIN_STABLE_COUNT        = 2;      // 连续稳定次数才触发（防瞬时假稳定）
    static constexpr double ZERO_MAX_KG = 8.0;                // 归零(硬件去皮)允许的最大净重(kg)，超过则硬件不支持
    int  m_stableCount  = 0;
    bool m_triggered    = false; // 防止重复触发

    // ==================== 设备序列号 ====================
    QString m_sn;       // 设备序列号 (启动时读取一次缓存)

    // ==================== 迟滞显示重量 (Hysteresis) ====================
    // 用于 QML 显示/计算/保存的统一重量。带迟滞区间防止临界点来回跳变：
    //   - 上跳阈值 = m_displayWeight + HYSTERESIS_THRESHOLD
    //   - 下跳阈值 = m_displayWeight - HYSTERESIS_THRESHOLD
    //   - 死区 (m_displayWeight - T, m_displayWeight + T) 内保持当前显示
    // 公式：HYSTERESIS_THRESHOLD = 0.005 (半档) + 0.002 (迟滞量) = 0.007
    // 注意：硬件真实值在 X.005 临界点漂移会导致常规四舍五入在 X.00/X.01 来回跳
    //       加迟滞后，需越过死区才跳变，UI 稳定。
    double m_displayWeight = 0.0;
    static constexpr double HYSTERESIS_THRESHOLD = 0.007;

    // ==================== 缓冲池 (生产者-消费者) ====================
    WeightSample   m_buffer;                    // 最新数据缓存（覆盖写）
    bool           m_bufferHasData = false;     // 是否有待消费数据
    QTimer        *m_consumeTimer = nullptr;    // 消费定时器
    static constexpr int CONSUME_INTERVAL_MS = 150;  // 消费间隔(ms)
};

#endif // WEIGHTSENSOR_H
