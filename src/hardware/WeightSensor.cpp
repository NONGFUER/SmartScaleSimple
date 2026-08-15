#include "WeightSensor.h"
#include "WeightSensorWorker.h"
#include <QDebug>
#include <QtMath>

// ============================================================================
// 构造 / 析构 — 创建并启动 Worker 线程
// ============================================================================

WeightSensor::WeightSensor(QObject *parent)
    : QObject(parent)
    , m_workerThread(new QThread(this))
    , m_worker(nullptr)
    , m_netWeight(0.0)
    , m_isStable(false)
    , m_zeroOffset(0.0)
    , m_tareWeight(0.0)
{
    // 创建 Worker 并移到独立线程
    m_worker = new WeightSensorWorker();
    m_worker->moveToThread(m_workerThread);

    // 线程结束时自动清理 Worker
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    // Worker → GUI: 数据就绪 (QueuedConnection, 跨线程安全)
    connect(m_worker, &WeightSensorWorker::weightDataReady,
            this,     &WeightSensor::onWeightDataReady);
    // Worker → GUI: 去皮完成
    connect(m_worker, &WeightSensorWorker::tareDone,
            this,     &WeightSensor::onTareDone);
    // Worker → GUI: 校准完成
    connect(m_worker, &WeightSensorWorker::calibrateDone,
            this,     &WeightSensor::onCalibrateDone);

    // GUI → Worker: 去皮请求 (QueuedConnection)
    connect(this, &WeightSensor::requestTare,
            m_worker, &WeightSensorWorker::doTare);
    // GUI → Worker: 校准请求 (QueuedConnection)
    connect(this, &WeightSensor::requestCalibrate,
            m_worker, &WeightSensorWorker::doCalibrate);
    // GUI → Worker: 读 SN 请求 (QueuedConnection)
    connect(this, &WeightSensor::requestReadSN,
            m_worker, &WeightSensorWorker::doReadSN);
    // Worker → GUI: SN 读取完成
    connect(m_worker, &WeightSensorWorker::snReady,
            this,     &WeightSensor::onSnReady);

    // 启动线程 → Worker 初始化串口 → 启动轮询定时器
    // 使用 QueuedConnection 确保 startPolling() 在 Worker 线程中执行
    connect(m_workerThread, &QThread::started, m_worker, [this]() {
        m_worker->startPolling();
        // 启动后自动读取一次设备序列号
        Q_EMIT requestReadSN();
    }, Qt::QueuedConnection);

    m_workerThread->start();

    // 消费定时器：主线程空闲时从缓冲池取最新数据
    m_consumeTimer = new QTimer(this);
    m_consumeTimer->setInterval(CONSUME_INTERVAL_MS);
    connect(m_consumeTimer, &QTimer::timeout, this, &WeightSensor::consumeBuffer);
    m_consumeTimer->start();

    qDebug() << "[WeightSensor] 初始化完成, Worker线程已启动";
}

WeightSensor::~WeightSensor()
{
    m_workerThread->quit();
    m_workerThread->wait();
    qDebug() << "[WeightSensor] 已销毁";
}

// ============================================================================
// QML 属性访问器
// ============================================================================

double WeightSensor::netWeight() const
{
    // 负20g以内显示为0.00，不出现负数
    if (m_netWeight > -0.02 && m_netWeight < 0.0) {
        return 0.0;
    }
    return m_netWeight;
}
double WeightSensor::displayWeight() const
{
    return m_displayWeight;
}
bool WeightSensor::isStable()  const { return m_isStable; }
QString WeightSensor::sn()     const { return m_sn; }

// ============================================================================
// Q_INVOKABLE 操作
// ============================================================================

void WeightSensor::tare()
{
    qDebug() << "[WeightSensor] >>> 请求去皮...";
    Q_EMIT requestTare();  // 通过 QueuedConnection 发到 Worker 线程
}

void WeightSensor::zero()
{
    // 归零 = 硬件去皮，但 8kg 以上硬件不支持，软件层先拦截
    if (m_netWeight > ZERO_MAX_KG) {
        qWarning() << "[WeightSensor] 归零失败: 当前净重" << m_netWeight
                   << "kg >" << ZERO_MAX_KG << "kg, 硬件不支持去皮";
        Q_EMIT tareDone(false);
        return;
    }
    qDebug() << "[WeightSensor] >>> 请求归零(走硬件去皮), 当前净重" << m_netWeight << "kg";
    Q_EMIT requestTare();
}

// ============================================================================
// 校准 — 预留接口, 转发到 Worker 线程
//   使用流程: 1) 空载 → tare()  2) 放砝码 → calibrateHalf()/calibrateFull()
// ============================================================================

void WeightSensor::calibrateHalf()
{
    qDebug() << "[WeightSensor] >>> 请求半量程标定...";
    Q_EMIT requestCalibrate(2);  // CMD_CALIB_HALF
}

void WeightSensor::calibrateFull()
{
    qDebug() << "[WeightSensor] >>> 请求满量程标定...";
    Q_EMIT requestCalibrate(3);  // CMD_CALIB_FULL
}

// ============================================================================
// 读取设备序列号 — 通过信号转发到 Worker 线程异步执行
// ============================================================================

void WeightSensor::readSN()
{
    qDebug() << "[WeightSensor] >>> 请求读取设备序列号...";
    Q_EMIT requestReadSN();
}

// ============================================================================
// 接收 Worker 的称重数据 — 在 GUI 线程执行滤波 + 稳定检测 + 属性更新
// ============================================================================

void WeightSensor::onWeightDataReady(int32_t weight_g, uint16_t statusWord, int32_t adcRaw)
{
    m_buffer.weightKg   = weight_g / 1000.0;
    m_buffer.statusWord = statusWord;
    m_buffer.adcRaw     = adcRaw;
    m_bufferHasData = true;
}

// ============================================================================
// 接收 Worker 的去皮结果
// ============================================================================

void WeightSensor::onTareDone(bool ok)
{
    if (ok) {
        qDebug() << "[WeightSensor]去皮成功 (来自 Worker)";
    } else {
        qWarning() << "[WeightSensor]去皮失败 (来自 Worker)";
    }
    Q_EMIT tareDone(ok);
}

// ============================================================================
// 接收 Worker 的校准结果
// ============================================================================

void WeightSensor::onCalibrateDone(bool ok)
{
    if (ok) {
        qDebug() << "[WeightSensor] 校准命令已确认 (来自 Worker)";
    } else {
        qWarning() << "[WeightSensor] 校准失败 (来自 Worker)";
    }
    Q_EMIT calibrateDone(ok);
}

// ============================================================================
// 接收 Worker 的 SN 读取结果 — 在 GUI 线程更新缓存并通知 QML
// ============================================================================

void WeightSensor::onSnReady(const QString &sn)
{
    if (sn.isEmpty()) {
        qWarning() << "[WeightSensor] SN 读取失败 (来自 Worker)";
        return;
    }
    if (sn != m_sn) {
        m_sn = sn;
        Q_EMIT snChanged();
        qDebug() << "[WeightSensor] SN 已缓存:" << m_sn;
    }
}

// ============================================================================
// 缓冲池消费 — 低频定时器驱动, 执行滤波+稳定检测+属性更新
// ============================================================================

void WeightSensor::consumeBuffer()
{
    if (!m_bufferHasData) return;
    m_bufferHasData = false;

    double rawWeightKg = m_buffer.weightKg;
    uint16_t statusWord = m_buffer.statusWord;
    int32_t adcRaw = m_buffer.adcRaw;

    // 状态字位定义 (Feigong 标准, Worker 已将 V2 旧位归一化到此处):
    //   Bit0=稳定  Bit1=过载  Bit2=负重  Bit3=去皮
    bool hwStable = (statusWord & 0x01);
    bool hwTared  = (statusWord & 0x08);

    //qDebug().nospace() << "[Scale] raw=" << rawWeightKg << "kg"
                      // << " ADC=" << adcRaw
                       //<< QString(" status=0x%1").arg(statusWord, 4, 16, QChar('0'))
                       //<< (hwStable ? " [HW:稳定]" : " [HW:波动]")
                       //<< (hwTared  ? " [去皮]" : "")
                       //<< ((statusWord & 0x04) ? " 负重" : "");

    double newNetWeight = rawWeightKg - m_zeroOffset - m_tareWeight;

    // 更新稳定状态（直接使用硬件标志）
    if (hwStable != m_isStable) {
        m_isStable = hwStable;
        Q_EMIT stableChanged();
    }

    // 不稳定时重置计数和触发标志
    if (!hwStable) {
        m_stableCount = 0;
        m_triggered = false;
    }

    // 硬件判定稳定时：累计连续稳定次数 + 重量阈值双重校验
    if (hwStable && !m_triggered) {
        // 第一道门槛：重量必须超过最小阈值(150g)，过滤微小数据
        if (std::abs(newNetWeight) > MIN_TRIGGER_WEIGHT_KG) {
            m_stableCount++;
            // 第二道门槛：连续足够次数的稳定才触发（防瞬时假稳定）
            if (m_stableCount >= MIN_STABLE_COUNT) {
                Q_EMIT stableTriggered();
                m_triggered = true;
                qDebug() << "[Scale] *** stableTriggered! *** weight=" << newNetWeight << "kg"
                         << "stableCount=" << m_stableCount;
            }
        } else {
            // 重量在阈值以下时重置计数（微小波动不累积）
            m_stableCount = 0;
        }
    }

    // 变化量 > 0.001kg 才刷新 UI
    if (std::abs(newNetWeight - m_netWeight) > 0.001) {
        m_netWeight = newNetWeight;

        // ---- 迟滞算法（Hysteresis）更新显示重量 ----
        // 用 netWeight() getter 取值（已含负20g→0 钳位），与 QML 看到一致
        const double raw   = netWeight();
        const double round_ = std::round(raw * 100.0) / 100.0;  // 常规四舍五入到 2 位小数
        double newDisplay = m_displayWeight;                    // 默认保持

        if (round_ > m_displayWeight) {
            // 增加方向：需 raw 越过 (当前显示 + 0.007) 才跳到上一档
            if (raw > m_displayWeight + HYSTERESIS_THRESHOLD) {
                newDisplay = round_;
            }
        } else if (round_ < m_displayWeight) {
            // 减少方向：需 raw 越过 (当前显示 - 0.007) 才跳到下一档
            if (raw < m_displayWeight - HYSTERESIS_THRESHOLD) {
                newDisplay = round_;
            }
        }
        // round_ == m_displayWeight：保持在死区内，不更新

        if (std::abs(newDisplay - m_displayWeight) > 0.0001) {
            m_displayWeight = newDisplay;
            Q_EMIT displayWeightChanged();
        }

        Q_EMIT weightChanged();
    }
}
