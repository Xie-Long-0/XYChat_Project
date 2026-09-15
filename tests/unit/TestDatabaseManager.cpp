#include <QtTest/QtTest>
#include <QSet>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "database/DatabaseManager.h"

class TestDatabaseManager : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // 迁移测试
    void migrationCreatesAllTables();

    // 用户管理测试
    void registerAndRetrieveUser();
    void duplicateUsernameFails();
    void userExistsReturnsCorrectly();

    // Session 测试
    void createAndRetrieveSession();
    void deleteSessionWorks();
    void sessionExpiryIsSet();

    // 登录审计测试
    void recordAndCountFailedLogins();

    // 设备管理测试
    void registerAndListDevices();

    // M3 联系人测试
    void addAndListContacts();
    void contactIsBidirectional();

    // M3 会话与消息测试
    void createPrivateConversation();
    void sendAndRetrieveMessages();
    void syncMessagesAfterId();
    void unreadCountWorks();
    void markMessagesAsRead();

    // M5.5 安全加固测试
    void v4TablesExist();
    void sessionByIdContainsTokenHash();
    // P1 安全加固（2026-09-02）：validateSession 逐请求回查 sessions 表依赖的会话契约
    void sessionByIdReflectsDeletionAndExpiry();
    void conversationMembershipAuthorization();
    void messageAccessAuthorization();
    void clientMessageIdDeduplicates();
    void receiptsAggregatePerRecipient();
    void readCursorOnlyMovesForward();
    void syncEventsCursorWorks();
    // M9: sync_events 保留清理与已读游标事件
    void pruneSyncEventsPrunesExpiredAndAdvancesWatermark();
    void readCursorEventRoundTrips();

    // M6 端到端加密密钥管理测试
    void v5TablesExist();
    void identityKeyUpsertAndRetrieve();
    void prekeyUploadAndCount();
    void prekeyClaimIsOncePerDevice();
    void claimedPrekeyValidationAndConsumption();
    void removeDeviceClearsKeyMaterial();
    void identityKeyChangePurgesStalePrekeys();

    // M7a 群聊数据层测试
    void groupMigrationAddsNameAndRoleColumns();
    void createGroupInsertsOwnerAndMembers();
    void addGroupMembersSkipsDuplicates();
    void removeGroupMemberDeletesRow();
    void groupRoleReportsOwnershipAndNonMember();
    void getConversationsForUserIncludesGroupWithNameAndMemberCount();
    // M7a 子任务二：fan-out 与回执聚合数据支撑
    void groupMemberIdsAndCounts();
    void groupMessageReceiptCounts();

    // M9 特性栈：置顶/免打扰与消息编辑/删除
    void v9ColumnsExist();
    void setAndGetConversationPrefs();
    void prefsBackfillInConversationsList();
    void editMessageUpdatesContentAndTimestamp();
    void deleteMessageSoftDeletesIdempotently();
    // M10: 会话整表硬删除（回执/消息/成员/会话行全部清除）
    void deleteConversationRemovesAllRelatedRows();

    // M8 文件元数据、票据与访问控制
    void v10TablesAndColumnsExist();
    void fileRecordCreateAndRetrieve();
    void fileStatusTransitionsAreGuarded();
    void uploadingCountTracksActiveUploads();
    void staleUploadsAreSelectableByAge();
    void createFileRecordEnforcesQuotaAtomically();
    void terminalFilesAreSelectableForReaping();
    void fileTicketIssueValidateConsume();
    void fileTicketRejectsExpiryAndPrunes();
    void fileAccessRequiresMessageReference();
    void messageCarriesFileIdAcrossReadPaths();
    void deleteFileRecordRefusesReferencedFile();
    void unreferencedReadyFilesAreReclaimable();
    void sendMessageGuardsFileReadyStateAtomically();

private:
    DatabaseManager *m_db = nullptr;
    QString m_connectionName;
};

void TestDatabaseManager::initTestCase()
{
    m_connectionName = QString("test_db_%1").arg(QDateTime::currentMSecsSinceEpoch());
    m_db = new DatabaseManager(m_connectionName);

    // 使用内存数据库方便测试
    // 先手动打开
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    db.setDatabaseName(":memory:");
    QVERIFY(db.open());

    // 运行迁移
    QVERIFY(m_db->initialize());
}

void TestDatabaseManager::cleanupTestCase()
{
    delete m_db;
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName);
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

// 迁移
void TestDatabaseManager::migrationCreatesAllTables()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);

    // 检查所有表存在
    QVERIFY(q.exec("SELECT name FROM sqlite_master WHERE type='table'"));
    QStringList tables;
    while (q.next()) {
        tables << q.value(0).toString();
    }

    QVERIFY(tables.contains("users"));
    QVERIFY(tables.contains("devices"));
    QVERIFY(tables.contains("sessions"));
    QVERIFY(tables.contains("login_audit"));
    QVERIFY(tables.contains("contacts"));
    QVERIFY(tables.contains("conversations"));
    QVERIFY(tables.contains("conversation_members"));
    QVERIFY(tables.contains("messages"));
    QVERIFY(tables.contains("schema_version"));
}

void TestDatabaseManager::v4TablesExist()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    QVERIFY(q.exec("SELECT name FROM sqlite_master WHERE type='table'"));
    QStringList tables;
    while (q.next()) {
        tables << q.value(0).toString();
    }
    QVERIFY(tables.contains("message_receipts"));
    QVERIFY(tables.contains("sync_events"));
}

void TestDatabaseManager::sessionByIdContainsTokenHash()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    const qint64 sid = m_db->createSession(user->id, "dev-renew",
                                           "tokenhash-renew", "127.0.0.1");
    QVERIFY(sid > 0);

    auto session = m_db->getSessionById(sid);
    QVERIFY(session.has_value());
    QCOMPARE(session->tokenHash, QString("tokenhash-renew"));
    QCOMPARE(session->deviceId, QString("dev-renew"));
}

// P1 安全加固（2026-09-02）：validateSession 逐请求回查 sessions 表，
// 依赖两项契约——会话删除后 getSessionById 立即返回空（登出/终止/续期换代即失效），
// 且 expiresAt 以可解析的 ISO 格式存储并落在未来（过期判定不会 fail-open）。
void TestDatabaseManager::sessionByIdReflectsDeletionAndExpiry()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    const qint64 sid = m_db->createSession(user->id, "dev-p1",
                                           "tokenhash-p1", "127.0.0.1", 3600);
    QVERIFY(sid > 0);

    // 契约一：expiresAt 可被 Qt::ISODate 解析且在未来（新鲜会话未过期）
    auto session = m_db->getSessionById(sid);
    QVERIFY(session.has_value());
    QCOMPARE(session->userId, user->id);
    const QDateTime expiresAt = QDateTime::fromString(session->expiresAt, Qt::ISODate);
    QVERIFY(expiresAt.isValid());
    QVERIFY(expiresAt > QDateTime::currentDateTimeUtc());

    // 契约二：删除会话后 getSessionById 立即返回空（token 被终止/登出后存量连接失效）
    QVERIFY(m_db->deleteSession(sid));
    QVERIFY(!m_db->getSessionById(sid).has_value());
}

void TestDatabaseManager::conversationMembershipAuthorization()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    // 局外用户
    const qint64 outsiderId = m_db->registerUser("outsider", "", "", "hash3");
    QVERIFY(outsiderId > 0);

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    QVERIFY(convId > 0);

    // 成员可访问，非成员被拒绝
    QVERIFY(m_db->isConversationMember(convId, user1->id));
    QVERIFY(m_db->isConversationMember(convId, user2->id));
    QVERIFY(!m_db->isConversationMember(convId, outsiderId));
}

void TestDatabaseManager::messageAccessAuthorization()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    auto outsider = m_db->getUserByUsername("outsider");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());
    QVERIFY(outsider.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    const qint64 msgId = m_db->sendMessage(convId, user1->id, "auth check msg");
    QVERIFY(msgId > 0);

    QVERIFY(m_db->canAccessMessage(msgId, user1->id));
    QVERIFY(m_db->canAccessMessage(msgId, user2->id));
    QVERIFY(!m_db->canAccessMessage(msgId, outsider->id));
}

void TestDatabaseManager::clientMessageIdDeduplicates()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);

    // 首次发送
    const qint64 first = m_db->sendMessage(convId, user1->id, "retry-safe",
                                           "text", "client-key-1", "deviceA");
    QVERIFY(first > 0);

    // 同一设备重试相同幂等键：返回同一消息，不重复写入
    const qint64 retry = m_db->sendMessage(convId, user1->id, "retry-safe",
                                           "text", "client-key-1", "deviceA");
    QCOMPARE(retry, first);

    // 不同设备相同幂等键视为不同消息
    const qint64 otherDevice = m_db->sendMessage(convId, user1->id, "other device",
                                                 "text", "client-key-1", "deviceB");
    QVERIFY(otherDevice > 0);
    QVERIFY(otherDevice != first);

    auto byKey = m_db->getMessageByClientKey(user1->id, "deviceA", "client-key-1");
    QVERIFY(byKey.has_value());
    QCOMPARE(byKey->id, first);
}

void TestDatabaseManager::receiptsAggregatePerRecipient()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    const qint64 msgId = m_db->sendMessage(convId, user1->id, "receipt test");
    QVERIFY(msgId > 0);

    QCOMPARE(m_db->receiptCount(msgId, "delivered"), 0);
    QCOMPARE(m_db->receiptCount(msgId, "read"), 0);

    // user2 的两台设备先后送达
    QVERIFY(m_db->recordMessageReceipt(msgId, user2->id, "dev1", "delivered"));
    QVERIFY(m_db->recordMessageReceipt(msgId, user2->id, "dev2", "delivered"));
    QCOMPARE(m_db->receiptCount(msgId, "delivered"), 2);
    QCOMPARE(m_db->receiptCount(msgId, "read"), 0);

    // 其中一台已读
    QVERIFY(m_db->recordMessageReceipt(msgId, user2->id, "dev1", "read"));
    QCOMPARE(m_db->receiptCount(msgId, "read"), 1);

    // 重复回执不重复计数
    QVERIFY(m_db->recordMessageReceipt(msgId, user2->id, "dev1", "read"));
    QCOMPARE(m_db->receiptCount(msgId, "read"), 1);

    // 非法状态被拒绝
    QVERIFY(!m_db->recordMessageReceipt(msgId, user2->id, "dev1", "bogus"));
}

void TestDatabaseManager::readCursorOnlyMovesForward()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    const qint64 m1 = m_db->sendMessage(convId, user1->id, "cursor msg 1");
    const qint64 m2 = m_db->sendMessage(convId, user1->id, "cursor msg 2");
    QVERIFY(m2 > m1);

    QVERIFY(m_db->updateMemberReadCursor(convId, user2->id, m2));
    // 回退到更早的消息不应生效
    QVERIFY(m_db->updateMemberReadCursor(convId, user2->id, m1));

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare("SELECT last_read_message_id FROM conversation_members "
              "WHERE conversation_id = ? AND user_id = ?");
    q.addBindValue(convId);
    q.addBindValue(user2->id);
    QVERIFY(q.exec() && q.next());
    QCOMPARE(q.value(0).toLongLong(), m2);
}

void TestDatabaseManager::syncEventsCursorWorks()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    QVERIFY(m_db->appendSyncEvent(user1->id, "message", "{\"a\":1}") > 0);
    QVERIFY(m_db->appendSyncEvent(user1->id, "receipt", "{\"b\":2}") > 0);
    // 他人事件不可见
    QVERIFY(m_db->appendSyncEvent(user2->id, "message", "{\"c\":3}") > 0);

    auto all = m_db->getSyncEvents(user1->id, 0);
    QCOMPARE(all.size(), 2);
    QCOMPARE(all[0].eventType, QString("message"));
    QCOMPARE(all[1].eventType, QString("receipt"));
    QVERIFY(all[1].seq > all[0].seq);

    // 游标之后无新事件
    auto none = m_db->getSyncEvents(user1->id, all.last().seq);
    QCOMPARE(none.size(), 0);

    // limit 生效
    auto limited = m_db->getSyncEvents(user1->id, 0, 1);
    QCOMPARE(limited.size(), 1);
}

// M9: sync_events 保留清理——过期事件被删、水位线前进、新事件保留
void TestDatabaseManager::pruneSyncEventsPrunesExpiredAndAdvancesWatermark()
{
    auto user1 = m_db->getUserByUsername("testuser");
    QVERIFY(user1.has_value());

    const qint64 baseWatermark = m_db->prunedBelowSeq();

    // 直接插入两条过期事件（created_at 早于保留期）与一条新事件
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName);
        QSqlQuery q(db);
        q.prepare("INSERT INTO sync_events (user_id, event_type, payload, created_at) "
                  "VALUES (?, 'message', '{}', datetime('now', '-40 days'))");
        q.addBindValue(user1->id);
        QVERIFY(q.exec());
        QVERIFY(q.exec());
    }
    const qint64 freshSeq = m_db->appendSyncEvent(user1->id, "message", "{\"fresh\":true}");
    QVERIFY(freshSeq > 0);
    QCOMPARE(m_db->maxSyncEventSeq(), freshSeq);

    // 保留 30 天：删除 40 天前的两条，水位线前进，新事件保留
    QCOMPARE(m_db->pruneSyncEvents(30), 2);
    QVERIFY(m_db->prunedBelowSeq() > baseWatermark);
    QVERIFY(m_db->prunedBelowSeq() < freshSeq);

    // 新事件仍可拉取
    const auto after = m_db->getSyncEvents(user1->id, freshSeq - 1, 10);
    QCOMPARE(after.size(), 1);
    QCOMPARE(after.first().seq, freshSeq);

    // 再次清理无过期事件：返回 0，水位线不回退
    const qint64 watermark = m_db->prunedBelowSeq();
    QCOMPARE(m_db->pruneSyncEvents(30), 0);
    QCOMPARE(m_db->prunedBelowSeq(), watermark);
}

// M9: 已读游标事件写入已读者自身事件流，供其其他设备同步
void TestDatabaseManager::readCursorEventRoundTrips()
{
    auto user1 = m_db->getUserByUsername("testuser");
    QVERIFY(user1.has_value());

    const qint64 seq = m_db->appendSyncEvent(
        user1->id, "read_cursor", "{\"conversationId\":7,\"readMessageId\":42}");
    QVERIFY(seq > 0);

    const auto events = m_db->getSyncEvents(user1->id, seq - 1, 10);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().eventType, QString("read_cursor"));
    QVERIFY(events.first().payload.contains("\"readMessageId\":42"));
}

// 用户管理
void TestDatabaseManager::registerAndRetrieveUser()
{
    const qint64 id = m_db->registerUser(
        "testuser",
        "test@example.com",
        "13800000000",
        "v1:100000:aabb:aabb");

    QVERIFY(id > 0);

    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());
    QCOMPARE(user->username, "testuser");
    QCOMPARE(user->email, "test@example.com");
    QCOMPARE(user->phone, "13800000000");
}

void TestDatabaseManager::duplicateUsernameFails()
{
    const qint64 id = m_db->registerUser(
        "testuser", {}, {}, "hash");
    QCOMPARE(id, static_cast<qint64>(-1));
}

void TestDatabaseManager::userExistsReturnsCorrectly()
{
    QVERIFY(m_db->userExists("testuser"));
    QVERIFY(!m_db->userExists("nonexistent"));
}

// Session
void TestDatabaseManager::createAndRetrieveSession()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    const qint64 sessionId = m_db->createSession(
        user->id, "device1", "tokenhash1",
        "127.0.0.1");

    QVERIFY(sessionId > 0);

    auto session = m_db->getSessionByTokenHash("tokenhash1");
    QVERIFY(session.has_value());
    QCOMPARE(session->userId, user->id);
    QCOMPARE(session->deviceId, "device1");
}

void TestDatabaseManager::deleteSessionWorks()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    const qint64 sid = m_db->createSession(
        user->id, "device-del", "token-del",
        "127.0.0.1");
    QVERIFY(sid > 0);

    QVERIFY(m_db->deleteSession(sid));
    auto session = m_db->getSessionByTokenHash("token-del");
    QVERIFY(!session.has_value());
}

void TestDatabaseManager::sessionExpiryIsSet()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    const qint64 sid = m_db->createSession(
        user->id, "device-exp", "token-exp",
        "127.0.0.1", 3600);
    QVERIFY(sid > 0);

    auto session = m_db->getSessionByTokenHash("token-exp");
    QVERIFY(session.has_value());
    QVERIFY(!session->expiresAt.isEmpty());
}

// 登录审计
void TestDatabaseManager::recordAndCountFailedLogins()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    // 记录 3 次失败
    for (int i = 0; i < 3; ++i) {
        m_db->recordLoginAttempt(user->id, "192.168.1.1",
                                 false, "Wrong password");
    }

    const int ipCount = m_db->recentFailedLoginCount("192.168.1.1");
    QCOMPARE(ipCount, 3);

    const int userCount = m_db->recentFailedLoginCountForUser(user->id);
    QCOMPARE(userCount, 3);

    // 成功的不计入
    m_db->recordLoginAttempt(user->id, "192.168.1.1", true);
    QCOMPARE(m_db->recentFailedLoginCount("192.168.1.1"), 3);
}

// 设备管理
void TestDatabaseManager::registerAndListDevices()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    QVERIFY(m_db->registerDevice(user->id, "dev-001",
                                 "My Phone", "android"));

    auto devices = m_db->getDevicesByUserId(user->id);
    QCOMPARE(devices.size(), 1);
    QCOMPARE(devices[0].value("deviceId").toString(), "dev-001");
    QCOMPARE(devices[0].value("platform").toString(), "android");

    // UPSERT：重复注册同一设备不会增加数量
    QVERIFY(m_db->registerDevice(user->id, "dev-001",
                                 "My Phone Updated", "android"));
    devices = m_db->getDevicesByUserId(user->id);
    QCOMPARE(devices.size(), 1);
}

// 联系人
void TestDatabaseManager::addAndListContacts()
{
    // 注册第二个用户
    const qint64 id2 = m_db->registerUser("user2", "u2@test.com", "", "hash2");
    QVERIFY(id2 > 0);

    auto user1 = m_db->getUserByUsername("testuser");
    QVERIFY(user1.has_value());

    // 添加联系人
    QVERIFY(m_db->addContact(user1->id, id2));

    auto contacts = m_db->getContacts(user1->id);
    QCOMPARE(contacts.size(), 1);
    QCOMPARE(contacts[0].contactUserId, id2);
    QCOMPARE(contacts[0].contactUsername, "user2");
}

void TestDatabaseManager::contactIsBidirectional()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    // user2 的联系人列表也应包含 user1
    auto contacts2 = m_db->getContacts(user2->id);
    QCOMPARE(contacts2.size(), 1);
    QCOMPARE(contacts2[0].contactUserId, user1->id);

    QVERIFY(m_db->isContact(user1->id, user2->id));
    QVERIFY(m_db->isContact(user2->id, user1->id));
}

// M3 会话与消息
void TestDatabaseManager::createPrivateConversation()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    QVERIFY(convId > 0);

    // 重复调用返回相同会话
    const qint64 convId2 = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    QCOMPARE(convId2, convId);

    auto convs = m_db->getConversationsForUser(user1->id);
    QCOMPARE(convs.size(), 1);
    QCOMPARE(convs[0].peerUserId, user2->id);
    QCOMPARE(convs[0].peerUsername, "user2");
}

void TestDatabaseManager::sendAndRetrieveMessages()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);

    // 发送消息
    const qint64 msg1 = m_db->sendMessage(convId, user1->id, "Hello!");
    const qint64 msg2 = m_db->sendMessage(convId, user2->id, "Hi there!");
    QVERIFY(msg1 > 0);
    QVERIFY(msg2 > 0);
    QVERIFY(msg2 > msg1);

    // 获取消息
    auto messages = m_db->getMessages(convId);
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages[0].content, "Hello!");
    QCOMPARE(messages[0].senderId, user1->id);
    QCOMPARE(messages[1].content, "Hi there!");
    QCOMPARE(messages[1].senderId, user2->id);
}

void TestDatabaseManager::syncMessagesAfterId()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    auto allMsgs = m_db->getMessages(convId);
    QVERIFY(allMsgs.size() >= 2);

    const qint64 afterId = allMsgs.first().id;
    auto synced = m_db->syncMessages(convId, afterId);
    // 应该只返回 afterId 之后的消息
    for (const auto &m : synced) {
        QVERIFY(m.id > afterId);
    }
}

void TestDatabaseManager::unreadCountWorks()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);

    // user2 发送消息给 user1
    m_db->sendMessage(convId, user2->id, "msg for user1");

    const int unread = m_db->getUnreadCount(convId, user1->id);
    QVERIFY(unread >= 1);
}

void TestDatabaseManager::markMessagesAsRead()
{
    auto user1 = m_db->getUserByUsername("testuser");
    QVERIFY(user1.has_value());

    auto convs = m_db->getConversationsForUser(user1->id);
    QVERIFY(!convs.isEmpty());
    const qint64 convId = convs[0].id;

    // 标记为已读
    QVERIFY(m_db->updateMessagesReadStatus(convId, user1->id));

    // 已读数应为 0
    const int unread = m_db->getUnreadCount(convId, user1->id);
    QCOMPARE(unread, 0);
}

// M6: 端到端加密密钥管理
void TestDatabaseManager::v5TablesExist()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    QVERIFY(q.exec("SELECT name FROM sqlite_master WHERE type='table'"));
    QStringList tables;
    while (q.next()) {
        tables << q.value(0).toString();
    }
    QVERIFY(tables.contains("device_identity_keys"));
    QVERIFY(tables.contains("prekeys"));
}

void TestDatabaseManager::identityKeyUpsertAndRetrieve()
{
    const qint64 userId = m_db->registerUser("e2eeuser", "", "", "hash-e2ee");
    QVERIFY(userId > 0);

    QVERIFY(m_db->upsertIdentityKey(userId, "dev-a", "pubA"));
    auto keys = m_db->getIdentityKeysByUser(userId);
    QCOMPARE(keys.size(), 1);
    QCOMPARE(keys[0].deviceId, "dev-a");
    QCOMPARE(keys[0].identityPub, "pubA");

    // UPSERT：同设备更新公钥不新增记录
    QVERIFY(m_db->upsertIdentityKey(userId, "dev-a", "pubA2"));
    keys = m_db->getIdentityKeysByUser(userId);
    QCOMPARE(keys.size(), 1);
    QCOMPARE(keys[0].identityPub, "pubA2");

    // 空参数被拒绝
    QVERIFY(!m_db->upsertIdentityKey(userId, "", "pub"));
    QVERIFY(!m_db->upsertIdentityKey(userId, "dev-a", ""));
}

void TestDatabaseManager::prekeyUploadAndCount()
{
    auto user = m_db->getUserByUsername("e2eeuser");
    QVERIFY(user.has_value());

    QCOMPARE(m_db->prekeyCount(user->id, "dev-a"), 0);
    QCOMPARE(m_db->uploadPrekeys(user->id, "dev-a", {"pk1", "pk2", "pk3"}), 3);
    QCOMPARE(m_db->prekeyCount(user->id, "dev-a"), 3);

    // 非法参数
    QCOMPARE(m_db->uploadPrekeys(user->id, "", {"pk"}), -1);
    QCOMPARE(m_db->uploadPrekeys(user->id, "dev-a", {}), -1);
}

void TestDatabaseManager::prekeyClaimIsOncePerDevice()
{
    auto user = m_db->getUserByUsername("e2eeuser");
    QVERIFY(user.has_value());

    // 每设备认领一个：再加一台设备验证多设备各认领一个
    m_db->uploadPrekeys(user->id, "dev-b", {"pk-b1"});

    auto claimed = m_db->claimPrekeys(user->id);
    QCOMPARE(claimed.size(), 2);
    QSet<QString> claimedDevices;
    qint64 firstDevAPrekeyId = 0;
    for (const auto &c : claimed) {
        claimedDevices.insert(c.deviceId);
        QVERIFY(!c.prekeyPub.isEmpty());
        QVERIFY(c.prekeyId > 0);
        if (c.deviceId == "dev-a") firstDevAPrekeyId = c.prekeyId;
    }
    QVERIFY(claimedDevices.contains("dev-a"));
    QVERIFY(claimedDevices.contains("dev-b"));
    QVERIFY(firstDevAPrekeyId > 0);

    // 认领后余量递减（dev-a 3-1=2，dev-b 1-1=0）
    QCOMPARE(m_db->prekeyCount(user->id, "dev-a"), 2);
    QCOMPARE(m_db->prekeyCount(user->id, "dev-b"), 0);

    // 再次认领：dev-b 无库存，只剩 dev-a，且不会重复认领同一预密钥
    auto claimed2 = m_db->claimPrekeys(user->id);
    QCOMPARE(claimed2.size(), 1);
    QCOMPARE(claimed2[0].deviceId, "dev-a");
    QVERIFY(claimed2[0].prekeyId != firstDevAPrekeyId);

    // 同一预密钥不会被两次认领：继续认领直到耗尽
    auto claimed3 = m_db->claimPrekeys(user->id);
    QCOMPARE(claimed3.size(), 1);
    auto claimed4 = m_db->claimPrekeys(user->id);
    QVERIFY(claimed4.isEmpty());
}

void TestDatabaseManager::claimedPrekeyValidationAndConsumption()
{
    auto user = m_db->getUserByUsername("e2eeuser");
    QVERIFY(user.has_value());

    m_db->uploadPrekeys(user->id, "dev-a", {"pk-v1"});
    auto claimed = m_db->claimPrekeys(user->id);
    QVERIFY(!claimed.isEmpty());
    const ClaimedPrekey c = claimed.last();

    // claimed 状态可校验通过
    QVERIFY(m_db->validateClaimedPrekey(user->id, c.deviceId, c.prekeyId));
    // 错误的设备/用户/ID 被拒绝
    QVERIFY(!m_db->validateClaimedPrekey(user->id, "dev-x", c.prekeyId));
    QVERIFY(!m_db->validateClaimedPrekey(user->id + 999, c.deviceId, c.prekeyId));
    QVERIFY(!m_db->validateClaimedPrekey(user->id, c.deviceId, c.prekeyId + 999));

    // 消费后（used）不再可校验
    QCOMPARE(m_db->consumePrekeys({c.prekeyId}), 1);
    QVERIFY(!m_db->validateClaimedPrekey(user->id, c.deviceId, c.prekeyId));
    // 重复消费返回 0
    QCOMPARE(m_db->consumePrekeys({c.prekeyId}), 0);
}

void TestDatabaseManager::removeDeviceClearsKeyMaterial()
{
    auto user = m_db->getUserByUsername("e2eeuser");
    QVERIFY(user.has_value());

    m_db->registerDevice(user->id, "dev-c", "Phone", "android");
    m_db->upsertIdentityKey(user->id, "dev-c", "pubC");
    m_db->uploadPrekeys(user->id, "dev-c", {"pk-c1", "pk-c2"});
    QCOMPARE(m_db->prekeyCount(user->id, "dev-c"), 2);

    // 删除设备后密钥材料全部清除，无法再认领
    QVERIFY(m_db->removeDevice(user->id, "dev-c"));
    QCOMPARE(m_db->prekeyCount(user->id, "dev-c"), 0);
    bool found = false;
    for (const auto &k : m_db->getIdentityKeysByUser(user->id)) {
        if (k.deviceId == "dev-c") found = true;
    }
    QVERIFY(!found);
    QVERIFY(m_db->claimPrekeys(user->id).isEmpty());
    QVERIFY(m_db->removeDeviceKeys(user->id, "dev-c")); // 幂等删除
}

void TestDatabaseManager::identityKeyChangePurgesStalePrekeys()
{
    // 审查修复回归：身份公钥变更时，旧世代未消费预密钥必须废弃，
    // 否则发送方会认领到接收方无法解密的旧预密钥导致消息静默丢失
    const qint64 userId = m_db->registerUser("e2eerotate", "", "", "hash-rotate");
    QVERIFY(userId > 0);

    QVERIFY(m_db->upsertIdentityKey(userId, "dev-r", "gen1-pub"));
    QCOMPARE(m_db->uploadPrekeys(userId, "dev-r", {"gen1-pk1", "gen1-pk2"}), 2);
    QCOMPARE(m_db->prekeyCount(userId, "dev-r"), 2);

    // 相同公钥重复注册不清除预密钥
    QVERIFY(m_db->upsertIdentityKey(userId, "dev-r", "gen1-pub"));
    QCOMPARE(m_db->prekeyCount(userId, "dev-r"), 2);

    // 身份变更后旧预密钥全部废弃
    QVERIFY(m_db->upsertIdentityKey(userId, "dev-r", "gen2-pub"));
    QCOMPARE(m_db->prekeyCount(userId, "dev-r"), 0);
    QVERIFY(m_db->claimPrekeys(userId).isEmpty());

    // 新世代预密钥正常工作
    QCOMPARE(m_db->uploadPrekeys(userId, "dev-r", {"gen2-pk1"}), 1);
    QCOMPARE(m_db->claimPrekeys(userId).size(), 1);
}

// M7a: 群聊数据层
void TestDatabaseManager::groupMigrationAddsNameAndRoleColumns()
{
    // 主库：V7 列已存在
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    QVERIFY(q.exec("PRAGMA table_info(conversations)"));
    QStringList convCols;
    while (q.next()) {
        convCols << q.value(1).toString();
    }
    QVERIFY(convCols.contains("name"));

    QVERIFY(q.exec("PRAGMA table_info(conversation_members)"));
    QStringList memberCols;
    while (q.next()) {
        memberCols << q.value(1).toString();
    }
    QVERIFY(memberCols.contains("role"));

    // 模拟 V6 旧库：迁移后补齐列且存量数据完好
    const QString legacyConn = QString("test_v7_%1").arg(QDateTime::currentMSecsSinceEpoch());
    {
        QSqlDatabase legacy = QSqlDatabase::addDatabase("QSQLITE", legacyConn);
        legacy.setDatabaseName(":memory:");
        QVERIFY(legacy.open());
        QSqlQuery lq(legacy);
        QVERIFY(lq.exec(
            "CREATE TABLE schema_version ("
            "  version INTEGER PRIMARY KEY,"
            "  applied_at TEXT NOT NULL DEFAULT (datetime('now')))"));
        QVERIFY(lq.exec("INSERT INTO schema_version (version) VALUES (6)"));
        QVERIFY(lq.exec(
            "CREATE TABLE conversations ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  type TEXT NOT NULL DEFAULT 'private',"
            "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
            "  updated_at TEXT NOT NULL DEFAULT (datetime('now')))"));
        QVERIFY(lq.exec(
            "CREATE TABLE conversation_members ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  conversation_id INTEGER NOT NULL,"
            "  user_id INTEGER NOT NULL,"
            "  joined_at TEXT NOT NULL DEFAULT (datetime('now')),"
            "  last_read_message_id INTEGER DEFAULT 0,"
            "  UNIQUE(conversation_id, user_id))"));
        // V6 旧库已有 messages 表（V1 创建）；V9 迁移需对其 ALTER 加列
        QVERIFY(lq.exec(
            "CREATE TABLE messages ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  conversation_id INTEGER NOT NULL,"
            "  sender_id INTEGER NOT NULL,"
            "  content TEXT NOT NULL,"
            "  content_type TEXT NOT NULL DEFAULT 'text',"
            "  status TEXT NOT NULL DEFAULT 'sent',"
            "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
            "  client_message_id TEXT,"
            "  sender_device_id TEXT)"));
        QVERIFY(lq.exec("INSERT INTO conversations (type) VALUES ('private')"));
        QVERIFY(lq.exec("INSERT INTO conversation_members (conversation_id, user_id) VALUES (1, 1)"));
    }

    DatabaseManager legacyDb(legacyConn);
    QVERIFY(legacyDb.initialize());

    QSqlDatabase legacy = QSqlDatabase::database(legacyConn);
    QSqlQuery check(legacy);
    QVERIFY(check.exec("PRAGMA table_info(conversations)"));
    QStringList legacyConvCols;
    while (check.next()) {
        legacyConvCols << check.value(1).toString();
    }
    QVERIFY(legacyConvCols.contains("name"));

    QVERIFY(check.exec("PRAGMA table_info(conversation_members)"));
    QStringList legacyMemberCols;
    while (check.next()) {
        legacyMemberCols << check.value(1).toString();
    }
    QVERIFY(legacyMemberCols.contains("role"));

    // 存量成员数据完好，role 默认 member
    QVERIFY(check.exec("SELECT role, conversation_id, user_id FROM conversation_members"));
    QVERIFY(check.next());
    QCOMPARE(check.value(0).toString(), QString("member"));
    QCOMPARE(check.value(1).toLongLong(), 1LL);
    QCOMPARE(check.value(2).toLongLong(), 1LL);

    // V9 迁移补齐置顶/免打扰与编辑/删除列
    QVERIFY(check.exec("PRAGMA table_info(conversation_members)"));
    QStringList v9MemberCols;
    while (check.next()) {
        v9MemberCols << check.value(1).toString();
    }
    QVERIFY(v9MemberCols.contains("pinned"));
    QVERIFY(v9MemberCols.contains("muted"));

    QVERIFY(check.exec("PRAGMA table_info(messages)"));
    QStringList v9MsgCols;
    while (check.next()) {
        v9MsgCols << check.value(1).toString();
    }
    QVERIFY(v9MsgCols.contains("edited_at"));
    QVERIFY(v9MsgCols.contains("deleted"));

    // V10 迁移补齐消息的文件关联列
    QVERIFY(v9MsgCols.contains("file_id"));

    // 版本号推进到最新（V7 群迁移之后还有 V8 同步事件、V9 置顶/免打扰与
    // 编辑/删除、V10 文件元数据与票据）
    QVERIFY(check.exec("SELECT MAX(version) FROM schema_version"));
    QVERIFY(check.next());
    QCOMPARE(check.value(0).toInt(), 10);
}

void TestDatabaseManager::createGroupInsertsOwnerAndMembers()
{
    auto owner = m_db->getUserByUsername("testuser");
    auto member = m_db->getUserByUsername("user2");
    QVERIFY(owner.has_value());
    QVERIFY(member.has_value());

    // 重复、创建者自身与非法 ID 被自动过滤
    const qint64 convId = m_db->createGroup(owner->id, "项目群",
                                            {member->id, member->id, owner->id, -5});
    QVERIFY(convId > 0);

    auto conv = m_db->getConversation(convId);
    QVERIFY(conv.has_value());
    QCOMPARE(conv->type, QString("group"));
    QCOMPARE(conv->name, QString("项目群"));
    QCOMPARE(conv->memberCount, 2);

    QCOMPARE(m_db->getGroupMembers(convId).size(), 2);
    QCOMPARE(m_db->groupRole(convId, owner->id), QString("owner"));
    QCOMPARE(m_db->groupRole(convId, member->id), QString("member"));

    // 群名空白、创建者非法被拒绝
    QCOMPARE(m_db->createGroup(owner->id, "", {}), -1LL);
    QCOMPARE(m_db->createGroup(owner->id, "   ", {}), -1LL);
    QCOMPARE(m_db->createGroup(0, "群", {}), -1LL);
}

void TestDatabaseManager::addGroupMembersSkipsDuplicates()
{
    auto owner = m_db->getUserByUsername("testuser");
    auto member = m_db->getUserByUsername("user2");
    auto outsider = m_db->getUserByUsername("outsider");
    QVERIFY(owner.has_value());
    QVERIFY(member.has_value());
    QVERIFY(outsider.has_value());

    const qint64 convId = m_db->createGroup(owner->id, "邀请群", {member->id});
    QVERIFY(convId > 0);

    // 已在群中与非法 ID 被跳过，新成员加入成功
    QVERIFY(m_db->addGroupMembers(convId, {member->id, outsider->id, 0}));
    QCOMPARE(m_db->getGroupMembers(convId).size(), 3);
    QCOMPARE(m_db->groupRole(convId, outsider->id), QString("member"));

    // 全部重复的批量邀请：无变化也不报错
    QVERIFY(m_db->addGroupMembers(convId, {member->id, outsider->id}));
    QCOMPARE(m_db->getGroupMembers(convId).size(), 3);
}

void TestDatabaseManager::removeGroupMemberDeletesRow()
{
    auto owner = m_db->getUserByUsername("testuser");
    auto outsider = m_db->getUserByUsername("outsider");
    QVERIFY(owner.has_value());
    QVERIFY(outsider.has_value());

    const qint64 convId = m_db->createGroup(owner->id, "踢人群", {outsider->id});
    QVERIFY(convId > 0);

    QVERIFY(m_db->removeGroupMember(convId, outsider->id));
    QCOMPARE(m_db->groupRole(convId, outsider->id), QString());
    QCOMPARE(m_db->getGroupMembers(convId).size(), 1);

    // 重复移除返回 false
    QVERIFY(!m_db->removeGroupMember(convId, outsider->id));
}

void TestDatabaseManager::groupRoleReportsOwnershipAndNonMember()
{
    auto owner = m_db->getUserByUsername("testuser");
    auto member = m_db->getUserByUsername("user2");
    auto outsider = m_db->getUserByUsername("outsider");
    QVERIFY(owner.has_value());
    QVERIFY(member.has_value());
    QVERIFY(outsider.has_value());

    const qint64 convId = m_db->createGroup(owner->id, "角色群", {member->id});
    QVERIFY(convId > 0);

    QCOMPARE(m_db->groupRole(convId, owner->id), QString("owner"));
    QCOMPARE(m_db->groupRole(convId, member->id), QString("member"));
    QCOMPARE(m_db->groupRole(convId, outsider->id), QString());

    // 角色变更生效；非成员更新失败；非法角色取值被拒绝
    QVERIFY(m_db->updateMemberRole(convId, member->id, "admin"));
    QCOMPARE(m_db->groupRole(convId, member->id), QString("admin"));
    QVERIFY(!m_db->updateMemberRole(convId, outsider->id, "admin"));
    QVERIFY(!m_db->updateMemberRole(convId, member->id, "superadmin"));
    QCOMPARE(m_db->groupRole(convId, member->id), QString("admin"));
}

void TestDatabaseManager::getConversationsForUserIncludesGroupWithNameAndMemberCount()
{
    auto owner = m_db->getUserByUsername("testuser");
    auto member = m_db->getUserByUsername("user2");
    QVERIFY(owner.has_value());
    QVERIFY(member.has_value());

    const qint64 groupConvId = m_db->createGroup(owner->id, "列表群", {member->id});
    QVERIFY(groupConvId > 0);

    // 改群名生效
    QVERIFY(m_db->setGroupName(groupConvId, "改名后的群"));

    bool groupFound = false;
    const auto convs = m_db->getConversationsForUser(owner->id);
    for (const auto &ci : convs) {
        if (ci.id == groupConvId) {
            groupFound = true;
            QCOMPARE(ci.type, QString("group"));
            QCOMPARE(ci.name, QString("改名后的群"));
            QCOMPARE(ci.memberCount, 2);
        } else if (ci.type == "private") {
            // private 会话不受群聊字段影响
            QVERIFY(ci.name.isEmpty());
            QCOMPARE(ci.memberCount, 0);
        }
    }
    QVERIFY(groupFound);

    // setGroupName 拒绝 private 会话与空群名
    const qint64 privateConvId = m_db->getOrCreatePrivateConversation(owner->id, member->id);
    QVERIFY(privateConvId > 0);
    QVERIFY(!m_db->setGroupName(privateConvId, "不是群"));
    QVERIFY(!m_db->setGroupName(groupConvId, "  "));
}

void TestDatabaseManager::groupMemberIdsAndCounts()
{
    auto owner = m_db->getUserByUsername("testuser");
    auto member = m_db->getUserByUsername("user2");
    auto outsider = m_db->getUserByUsername("outsider");
    QVERIFY(owner.has_value());
    QVERIFY(member.has_value());
    QVERIFY(outsider.has_value());

    const qint64 convId = m_db->createGroup(owner->id, "分发群", {member->id});
    QVERIFY(convId > 0);

    // fan-out 用的成员 ID 列表
    const auto ids = m_db->getGroupMemberIds(convId);
    QCOMPARE(ids.size(), 2);
    QVERIFY(ids.contains(owner->id));
    QVERIFY(ids.contains(member->id));

    // 回执聚合的接收者总数（排除发送方）；非成员不排除任何成员
    QCOMPARE(m_db->memberCountExcluding(convId, owner->id), 1);
    QCOMPARE(m_db->memberCountExcluding(convId, outsider->id), 2);

    // 成员移除后接收者总数同步减少
    QVERIFY(m_db->removeGroupMember(convId, member->id));
    QCOMPARE(m_db->memberCountExcluding(convId, owner->id), 0);

    // usernameById：存在返回用户名，不存在返回空串
    QCOMPARE(m_db->usernameById(owner->id), QString("testuser"));
    QCOMPARE(m_db->usernameById(999999), QString());
}

void TestDatabaseManager::groupMessageReceiptCounts()
{
    auto owner = m_db->getUserByUsername("testuser");
    auto member = m_db->getUserByUsername("user2");
    auto outsider = m_db->getUserByUsername("outsider");
    QVERIFY(owner.has_value());
    QVERIFY(member.has_value());
    QVERIFY(outsider.has_value());

    const qint64 convId = m_db->createGroup(owner->id, "回执群", {member->id, outsider->id});
    QVERIFY(convId > 0);

    const qint64 msgId = m_db->sendMessage(convId, owner->id, "hello group",
                                           "text", "grp-key-1", "devA");
    QVERIFY(msgId > 0);

    // 接收者总数 = 除发送方外全体成员
    const int recipients = m_db->memberCountExcluding(convId, owner->id);
    QCOMPARE(recipients, 2);

    // 送达：单人回执不达成，全员回执才达成
    QCOMPARE(m_db->receiptCount(msgId, "delivered"), 0);
    QVERIFY(m_db->recordMessageReceipt(msgId, member->id, "devM", "delivered"));
    QCOMPARE(m_db->receiptCount(msgId, "delivered"), 1);
    QVERIFY(m_db->recordMessageReceipt(msgId, outsider->id, "devO", "delivered"));
    QCOMPARE(m_db->receiptCount(msgId, "delivered"), recipients);

    // 已读：同样按接收者计数聚合
    QVERIFY(m_db->recordMessageReceipt(msgId, member->id, "devM", "read"));
    QCOMPARE(m_db->receiptCount(msgId, "read"), 1);
    QVERIFY(m_db->recordMessageReceipt(msgId, outsider->id, "devO", "read"));
    QCOMPARE(m_db->receiptCount(msgId, "read"), recipients);

    // 按用户去重：同一用户多设备回执不重复计数，而逐设备计数保留原语义
    QVERIFY(m_db->recordMessageReceipt(msgId, member->id, "devM2", "read"));
    QCOMPARE(m_db->receiptUserCount(msgId, "read"), recipients);
    QCOMPARE(m_db->receiptCount(msgId, "read"), recipients + 1);

    // 系统消息（contentType=system）可正常入库与读回
    const qint64 sysId = m_db->sendMessage(convId, owner->id,
                                           "{\"event\":\"group_created\"}", "system");
    QVERIFY(sysId > 0);
    auto sysMsg = m_db->getMessage(sysId);
    QVERIFY(sysMsg.has_value());
    QCOMPARE(sysMsg->contentType, QString("system"));
    QCOMPARE(sysMsg->conversationId, convId);
}

// M9 特性栈：置顶/免打扰与消息编辑/删除

void TestDatabaseManager::v9ColumnsExist()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);

    QVERIFY(q.exec("PRAGMA table_info(conversation_members)"));
    QStringList memberCols;
    while (q.next()) {
        memberCols << q.value(1).toString();
    }
    QVERIFY(memberCols.contains("pinned"));
    QVERIFY(memberCols.contains("muted"));

    QVERIFY(q.exec("PRAGMA table_info(messages)"));
    QStringList msgCols;
    while (q.next()) {
        msgCols << q.value(1).toString();
    }
    QVERIFY(msgCols.contains("edited_at"));
    QVERIFY(msgCols.contains("deleted"));
}

void TestDatabaseManager::setAndGetConversationPrefs()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    auto outsider = m_db->getUserByUsername("outsider");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());
    QVERIFY(outsider.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    QVERIFY(convId > 0);

    // 默认未置顶/未免打扰
    auto initial = m_db->getConversationPrefs(convId, user1->id);
    QVERIFY(initial.has_value());
    QVERIFY(!initial->first);
    QVERIFY(!initial->second);

    // 置顶 + 免打扰
    QVERIFY(m_db->setConversationPrefs(convId, user1->id, true, true));
    auto updated = m_db->getConversationPrefs(convId, user1->id);
    QVERIFY(updated.has_value());
    QVERIFY(updated->first);
    QVERIFY(updated->second);

    // 偏好按成员隔离：user2 不受 user1 设置影响
    auto other = m_db->getConversationPrefs(convId, user2->id);
    QVERIFY(other.has_value());
    QVERIFY(!other->first);
    QVERIFY(!other->second);

    // 非成员设置不产生行，读取返回默认值
    QVERIFY(!m_db->setConversationPrefs(convId, outsider->id, true, false));
    auto outsiderPrefs = m_db->getConversationPrefs(convId, outsider->id);
    QVERIFY(outsiderPrefs.has_value());
    QVERIFY(!outsiderPrefs->first);
    QVERIFY(!outsiderPrefs->second);
}

void TestDatabaseManager::prefsBackfillInConversationsList()
{
    auto user1 = m_db->getUserByUsername("testuser");
    QVERIFY(user1.has_value());

    // 取 user1 的第一个私聊会话
    auto convs = m_db->getConversationsForUser(user1->id);
    QVERIFY(!convs.isEmpty());
    const qint64 convId = convs.first().id;

    QVERIFY(m_db->setConversationPrefs(convId, user1->id, true, false));

    bool found = false;
    const auto updated = m_db->getConversationsForUser(user1->id);
    for (const auto &ci : updated) {
        if (ci.id == convId) {
            found = true;
            QVERIFY(ci.pinned);
            QVERIFY(!ci.muted);
        }
    }
    QVERIFY(found);
}

void TestDatabaseManager::editMessageUpdatesContentAndTimestamp()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    const qint64 msgId = m_db->sendMessage(convId, user1->id, "original content");
    QVERIFY(msgId > 0);

    // 编辑前：无编辑时间、未删除
    auto before = m_db->getMessage(msgId);
    QVERIFY(before.has_value());
    QVERIFY(before->editedAt.isEmpty());
    QVERIFY(!before->deleted);

    // 编辑正文（保持 content 类型），编辑时间被记录
    QVERIFY(m_db->editMessage(msgId, "edited content", "text"));
    auto after = m_db->getMessage(msgId);
    QVERIFY(after.has_value());
    QCOMPARE(after->content, QString("edited content"));
    QVERIFY(!after->editedAt.isEmpty());
    QVERIFY(!after->deleted);

    // getMessages / syncMessages 回填 editedAt/deleted 字段
    const auto msgs = m_db->getMessages(convId);
    bool found = false;
    for (const auto &m : msgs) {
        if (m.id == msgId) {
            found = true;
            QCOMPARE(m.content, QString("edited content"));
            QVERIFY(!m.editedAt.isEmpty());
            QVERIFY(!m.deleted);
        }
    }
    QVERIFY(found);
}

void TestDatabaseManager::deleteMessageSoftDeletesIdempotently()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    const qint64 msgId = m_db->sendMessage(convId, user1->id, "delete me");
    QVERIFY(msgId > 0);

    // 软删除：正文清空、deleted 置 1，墓碑保留 messageId/发送者/时间
    QVERIFY(m_db->deleteMessage(msgId));
    auto deleted = m_db->getMessage(msgId);
    QVERIFY(deleted.has_value());
    QVERIFY(deleted->deleted);
    QVERIFY(deleted->content.isEmpty());
    QCOMPARE(deleted->id, msgId);
    QCOMPARE(deleted->senderId, user1->id);

    // 幂等：重复删除仍返回成功
    QVERIFY(m_db->deleteMessage(msgId));

    // 已删除消息不可再编辑（业务层校验，数据层删除后 edited 状态不变）
    auto stillDeleted = m_db->getMessage(msgId);
    QVERIFY(stillDeleted.has_value());
    QVERIFY(stillDeleted->deleted);
}

// M10: 会话整表硬删除——回执、消息、成员、会话行按 FK 安全顺序全部清除；
// 非法 ID 返回 false；删除后同一对用户再发消息新建全新会话（旧数据不复现）
void TestDatabaseManager::deleteConversationRemovesAllRelatedRows()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value());
    QVERIFY(user2.has_value());

    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    QVERIFY(convId > 0);
    const qint64 m1 = m_db->sendMessage(convId, user1->id, "doomed 1");
    const qint64 m2 = m_db->sendMessage(convId, user2->id, "doomed 2");
    QVERIFY(m1 > 0 && m2 > 0);
    QVERIFY(m_db->recordMessageReceipt(m1, user2->id, "devB", "delivered"));

    // 前置：成员/消息均存在
    QVERIFY(m_db->isConversationMember(convId, user1->id));
    QVERIFY(m_db->isConversationMember(convId, user2->id));
    QVERIFY(m_db->getConversationMemberIds(convId).size() == 2);
    QVERIFY(m_db->getMessage(m1).has_value());

    QVERIFY(m_db->deleteConversation(convId));

    // 会话行、成员、消息全部消失
    QVERIFY(!m_db->getConversation(convId).has_value());
    QVERIFY(!m_db->isConversationMember(convId, user1->id));
    QVERIFY(!m_db->isConversationMember(convId, user2->id));
    QVERIFY(m_db->getConversationMemberIds(convId).isEmpty());
    QVERIFY(!m_db->getMessage(m1).has_value());
    QVERIFY(!m_db->getMessage(m2).has_value());

    // 回执经显式子查询删除（直接查库确认，不依赖 foreign_keys 级联）
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare("SELECT COUNT(*) FROM message_receipts WHERE message_id = ?");
    q.addBindValue(m1);
    QVERIFY(q.exec() && q.next());
    QCOMPARE(q.value(0).toInt(), 0);

    // 非法 ID 返回 false；重复删除已删会话仍成功（0 行，事务提交）
    QVERIFY(!m_db->deleteConversation(0));
    QVERIFY(!m_db->deleteConversation(-1));
    QVERIFY(m_db->deleteConversation(convId));

    // 删除后同一对用户再建会话：全新 ID、无历史消息
    const qint64 freshConvId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    QVERIFY(freshConvId > 0);
    QVERIFY(freshConvId != convId);
    QVERIFY(m_db->getMessages(freshConvId).isEmpty());
}

namespace
{
// 造一条 uploading 状态的文件记录（分片口径自洽：chunkCount 片，末片为余量）。
// blobKey 由各用例传入且互不重复，避开 files.blob_key 的 UNIQUE 约束
qint64 makeUpload(DatabaseManager &db, qint64 uploaderId, const QString &blobKey)
{
    const qint64 chunkSize = 1024 * 1024;
    const int chunkCount = 2;
    const qint64 sizeBytes = chunkSize * (chunkCount - 1) + 4096;
    return db.createFileRecord(uploaderId, "deviceA", blobKey, sizeBytes, chunkSize,
                               chunkCount, QString(64, 'a'));
}
} // namespace

void TestDatabaseManager::v10TablesAndColumnsExist()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);

    QVERIFY(q.exec("SELECT name FROM sqlite_master WHERE type='table'"));
    QStringList tables;
    while (q.next()) {
        tables << q.value(0).toString();
    }
    QVERIFY(tables.contains("files"));
    QVERIFY(tables.contains("file_tickets"));

    QVERIFY(q.exec("PRAGMA table_info(files)"));
    QStringList fileCols;
    while (q.next()) {
        fileCols << q.value(1).toString();
    }
    for (const char *col : {"id", "blob_key", "uploader_id", "uploader_device_id",
                            "size_bytes", "chunk_size", "chunk_count", "sha256_hex",
                            "status", "created_at", "completed_at"}) {
        QVERIFY2(fileCols.contains(col), col);
    }

    QVERIFY(q.exec("PRAGMA table_info(file_tickets)"));
    QStringList ticketCols;
    while (q.next()) {
        ticketCols << q.value(1).toString();
    }
    for (const char *col : {"id", "ticket_hash", "file_id", "user_id", "kind",
                            "used", "expires_at", "created_at"}) {
        QVERIFY2(ticketCols.contains(col), col);
    }

    // 服务端只存摘要，绝不存票据明文
    QVERIFY(!ticketCols.contains("ticket"));

    QVERIFY(q.exec("SELECT MAX(version) FROM schema_version"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 10);
}

void TestDatabaseManager::fileRecordCreateAndRetrieve()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    // 入参兜底：非正数与空串一律拒绝
    QCOMPARE(m_db->createFileRecord(0, "d", "blob", 1024, 512, 2, "sha"), qint64(-1));
    QCOMPARE(m_db->createFileRecord(user->id, "d", QString(), 1024, 512, 2, "sha"),
             qint64(-1));
    QCOMPARE(m_db->createFileRecord(user->id, "d", "blob", 0, 512, 2, "sha"), qint64(-1));
    QCOMPARE(m_db->createFileRecord(user->id, "d", "blob", -1, 512, 2, "sha"), qint64(-1));
    QCOMPARE(m_db->createFileRecord(user->id, "d", "blob", 1024, 0, 2, "sha"), qint64(-1));
    QCOMPARE(m_db->createFileRecord(user->id, "d", "blob", 1024, 512, 0, "sha"), qint64(-1));
    QCOMPARE(m_db->createFileRecord(user->id, "d", "blob", 1024, 512, 2, QString()),
             qint64(-1));

    const qint64 fileId = makeUpload(*m_db, user->id, "a1b2c3d4");
    QVERIFY(fileId > 0);

    const auto rec = m_db->getFileRecord(fileId);
    QVERIFY(rec.has_value());
    QCOMPARE(rec->id, fileId);
    QCOMPARE(rec->blobKey, QString("a1b2c3d4"));
    QCOMPARE(rec->uploaderId, user->id);
    QCOMPARE(rec->uploaderDeviceId, QString("deviceA"));
    QCOMPARE(rec->status, QString("uploading"));
    QCOMPARE(rec->chunkCount, 2);
    QCOMPARE(rec->chunkSize, qint64(1024 * 1024));
    QCOMPARE(rec->sizeBytes, qint64(1024 * 1024) + 4096);
    QVERIFY(!rec->createdAt.isEmpty());
    // 未完成时不得有完成时间戳
    QVERIFY(rec->completedAt.isEmpty());

    QVERIFY(!m_db->getFileRecord(fileId + 1000000).has_value());
    QVERIFY(!m_db->getFileRecord(0).has_value());
    QVERIFY(!m_db->getFileRecord(-1).has_value());

    // blob_key UNIQUE：同一存储键不得被两条记录争用（否则两个文件的分片
    // 会写进同一目录，组装时互相污染）
    QCOMPARE(m_db->createFileRecord(user->id, "deviceA", "a1b2c3d4", 1024, 512, 2,
                                    QString(64, 'b')),
             qint64(-1));

    // 收尾：本行转终态。uploading 计数按用户聚合，留着会吃掉后续用例的并发配额
    QVERIFY(m_db->markFileCancelled(fileId));
}

void TestDatabaseManager::fileStatusTransitionsAreGuarded()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    // uploading -> ready 并写入 completed_at
    const qint64 readyId = makeUpload(*m_db, user->id, "st01");
    QVERIFY(readyId > 0);
    QVERIFY(m_db->markFileReady(readyId));
    auto rec = m_db->getFileRecord(readyId);
    QVERIFY(rec.has_value());
    QCOMPARE(rec->status, QString("ready"));
    QVERIFY(!rec->completedAt.isEmpty());

    // 终态不可再改：ready 不能被取消或标失败，否则已投递消息的附件会凭空消失
    QVERIFY(!m_db->markFileCancelled(readyId));
    QVERIFY(!m_db->markFileFailed(readyId));
    // 重复迁移同样失败：幂等由调用方先读状态实现，不在数据层隐藏
    QVERIFY(!m_db->markFileReady(readyId));
    QCOMPARE(m_db->getFileRecord(readyId)->status, QString("ready"));

    // uploading -> cancelled
    const qint64 cancelledId = makeUpload(*m_db, user->id, "st02");
    QVERIFY(m_db->markFileCancelled(cancelledId));
    QCOMPARE(m_db->getFileRecord(cancelledId)->status, QString("cancelled"));
    QVERIFY(!m_db->markFileReady(cancelledId));
    QVERIFY(!m_db->markFileCancelled(cancelledId));

    // uploading -> failed
    const qint64 failedId = makeUpload(*m_db, user->id, "st03");
    QVERIFY(m_db->markFileFailed(failedId));
    QCOMPARE(m_db->getFileRecord(failedId)->status, QString("failed"));
    QVERIFY(!m_db->markFileReady(failedId));

    // 不存在的记录与非法入参
    QVERIFY(!m_db->markFileReady(99999999));
    QVERIFY(!m_db->markFileReady(0));
    QVERIFY(!m_db->markFileCancelled(-1));
}

void TestDatabaseManager::uploadingCountTracksActiveUploads()
{
    auto user = m_db->getUserByUsername("testuser");
    auto other = m_db->getUserByUsername("user2");
    QVERIFY(user.has_value() && other.has_value());

    // 以差值断言：其他用例也会为同一用户创建上传记录，绝对值不可依赖
    const int base = m_db->uploadingCountForUser(user->id);
    QVERIFY(base >= 0);

    const qint64 a = makeUpload(*m_db, user->id, "uc01");
    const qint64 b = makeUpload(*m_db, user->id, "uc02");
    QVERIFY(a > 0 && b > 0);
    QCOMPARE(m_db->uploadingCountForUser(user->id), base + 2);

    // 只数本人的在传文件（并发配额不得被他人占满）
    const int otherBase = m_db->uploadingCountForUser(other->id);
    QVERIFY(otherBase >= 0);

    QVERIFY(m_db->markFileReady(a));
    QCOMPARE(m_db->uploadingCountForUser(user->id), base + 1);
    QVERIFY(m_db->markFileCancelled(b));
    QCOMPARE(m_db->uploadingCountForUser(user->id), base);
    QCOMPARE(m_db->uploadingCountForUser(other->id), otherBase);

    QCOMPARE(m_db->uploadingCountForUser(0), 0);
    QCOMPARE(m_db->uploadingCountForUser(-1), 0);
}

void TestDatabaseManager::staleUploadsAreSelectableByAge()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    const qint64 fileId = makeUpload(*m_db, user->id, "sl01");
    QVERIFY(fileId > 0);

    // 刚创建的上传不算超期
    for (const FileRecord &rec : m_db->getStaleUploads(48)) {
        QVERIFY(rec.id != fileId);
    }

    // 人工把 created_at 拨回 72 小时前，模拟客户端崩溃后遗留的未完成上传
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare("UPDATE files SET created_at = datetime('now', '-72 hours') WHERE id = ?");
    q.addBindValue(fileId);
    QVERIFY(q.exec());

    const QList<FileRecord> stale = m_db->getStaleUploads(48);
    bool found = false;
    for (const FileRecord &rec : stale) {
        // 回收任务只看 uploading：已完成的文件绝不能被当成遗留上传删掉
        QCOMPARE(rec.status, QString("uploading"));
        if (rec.id == fileId) {
            found = true;
            QCOMPARE(rec.blobKey, QString("sl01"));
        }
    }
    QVERIFY(found);

    // limit 生效
    QCOMPARE(m_db->getStaleUploads(48, 1).size(), qsizetype(1));
    // 非法入参
    QVERIFY(m_db->getStaleUploads(0).isEmpty());
    QVERIFY(m_db->getStaleUploads(-1).isEmpty());
    QVERIFY(m_db->getStaleUploads(48, 0).isEmpty());

    // 转 ready 后不再入选（即使 created_at 仍为 72 小时前）
    QVERIFY(m_db->markFileReady(fileId));
    for (const FileRecord &rec : m_db->getStaleUploads(48)) {
        QVERIFY(rec.id != fileId);
    }
}

void TestDatabaseManager::createFileRecordEnforcesQuotaAtomically()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    const qint64 chunkSize = 1024 * 1024;
    const qint64 sizeBytes = chunkSize + 4096;
    QList<qint64> created;

    // 以差值设定上限：其他用例也会为同一用户创建上传记录，绝对配额不可依赖
    const int limit = m_db->uploadingCountForUser(user->id) + 2;

    // 前两条成功，第三条被配额挡下并明确置位（区别于 SQL 错误）
    bool quota = false;
    const qint64 first = m_db->createFileRecord(user->id, "deviceA", "qt01", sizeBytes,
                                                chunkSize, 2, QString(64, 'a'), limit, &quota);
    QVERIFY(first > 0);
    QVERIFY(!quota);
    created.append(first);

    const qint64 second = m_db->createFileRecord(user->id, "deviceA", "qt02", sizeBytes,
                                                 chunkSize, 2, QString(64, 'a'), limit, &quota);
    QVERIFY(second > 0);
    QVERIFY(!quota);
    created.append(second);

    const qint64 third = m_db->createFileRecord(user->id, "deviceA", "qt03", sizeBytes,
                                                chunkSize, 2, QString(64, 'a'), limit, &quota);
    QCOMPARE(third, qint64(-1));
    // 配额已满时必须置位，否则调用方只能回误导性的 InternalError
    QVERIFY(quota);

    // 释放一个名额后即可继续（配额只看 uploading，ready 不占名额）
    QVERIFY(m_db->markFileReady(first));
    quota = true;
    const qint64 fourth = m_db->createFileRecord(user->id, "deviceA", "qt04", sizeBytes,
                                                 chunkSize, 2, QString(64, 'a'), limit, &quota);
    QVERIFY(fourth > 0);
    QVERIFY(!quota);
    created.append(fourth);

    // maxConcurrentUploads <= 0 表示不限制（既有调用口径不变）
    quota = true;
    const qint64 fifth = m_db->createFileRecord(user->id, "deviceA", "qt05", sizeBytes,
                                                chunkSize, 2, QString(64, 'a'), 0, &quota);
    QVERIFY(fifth > 0);
    QVERIFY(!quota);
    created.append(fifth);

    // 未传 quotaExceeded 也不得崩溃
    const qint64 sixth = m_db->createFileRecord(user->id, "deviceA", "qt06", sizeBytes,
                                                chunkSize, 2, QString(64, 'a'), 0);
    QVERIFY(sixth > 0);
    created.append(sixth);

    // 收尾：把本用例留下的 uploading 行转终态，不影响后续用例的计数
    for (qint64 id : created) {
        if (id != first) {
            QVERIFY(m_db->markFileCancelled(id));
        }
    }
}

void TestDatabaseManager::terminalFilesAreSelectableForReaping()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());

    const qint64 cancelledId = makeUpload(*m_db, user->id, "tm01");
    const qint64 failedId = makeUpload(*m_db, user->id, "tm02");
    const qint64 readyId = makeUpload(*m_db, user->id, "tm03");
    QVERIFY(cancelledId > 0 && failedId > 0 && readyId > 0);
    QVERIFY(m_db->markFileCancelled(cancelledId));
    QVERIFY(m_db->markFileFailed(failedId));
    QVERIFY(m_db->markFileReady(readyId));

    // 刚转终态的行不算超期
    for (const FileRecord &rec : m_db->getTerminalFiles(48)) {
        QVERIFY(rec.id != cancelledId && rec.id != failedId);
    }

    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare("UPDATE files SET created_at = datetime('now', '-72 hours') "
              "WHERE id IN (?, ?, ?)");
    q.addBindValue(cancelledId);
    q.addBindValue(failedId);
    q.addBindValue(readyId);
    QVERIFY(q.exec());

    bool sawCancelled = false;
    bool sawFailed = false;
    for (const FileRecord &rec : m_db->getTerminalFiles(48)) {
        // ready 行可能仍被消息引用，绝不能进入终态回收路径
        QVERIFY(rec.status == QLatin1String("cancelled")
                || rec.status == QLatin1String("failed"));
        QVERIFY(rec.id != readyId);
        if (rec.id == cancelledId) {
            sawCancelled = true;
        }
        if (rec.id == failedId) {
            sawFailed = true;
        }
    }
    QVERIFY(sawCancelled);
    QVERIFY(sawFailed);

    // 删除后不再入选（回收任务不会重复处理同一行）
    QVERIFY(m_db->deleteFileRecord(cancelledId));
    for (const FileRecord &rec : m_db->getTerminalFiles(48)) {
        QVERIFY(rec.id != cancelledId);
    }

    QVERIFY(m_db->getTerminalFiles(0).isEmpty());
    QVERIFY(m_db->getTerminalFiles(-1).isEmpty());
    QVERIFY(m_db->getTerminalFiles(48, 0).isEmpty());
    QCOMPARE(m_db->getTerminalFiles(48, 1).size(), qsizetype(1));

    QVERIFY(m_db->markFileFailed(readyId) == false); // 终态不可再改
    QVERIFY(m_db->deleteFileRecord(failedId));
}

void TestDatabaseManager::fileTicketIssueValidateConsume()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());
    const qint64 fileId = makeUpload(*m_db, user->id, "tk01");
    QVERIFY(m_db->markFileReady(fileId));

    // 本层不校验摘要的随机性（那是 FileCrypto 的职责），只校验形态与唯一性
    const QString hash = QString(64, 'c');
    QVERIFY(m_db->issueFileTicket(fileId, user->id, "download", hash, 300));

    const auto ticket = m_db->validateFileTicket(hash, "download");
    QVERIFY(ticket.has_value());
    QCOMPARE(ticket->fileId, fileId);
    QCOMPARE(ticket->userId, user->id);
    QCOMPARE(ticket->kind, QString("download"));
    QVERIFY(!ticket->used);
    QVERIFY(!ticket->expiresAt.isEmpty());
    QVERIFY(!ticket->createdAt.isEmpty());

    // 用途不符即拒绝：下载票据不能当上传票据用
    QVERIFY(!m_db->validateFileTicket(hash, "upload").has_value());
    // 未知摘要与空入参
    QVERIFY(!m_db->validateFileTicket(QString(64, 'd'), "download").has_value());
    QVERIFY(!m_db->validateFileTicket(QString(), "download").has_value());
    QVERIFY(!m_db->validateFileTicket(hash, QString()).has_value());

    // 消费后不可再用（一次性语义）
    QVERIFY(m_db->markFileTicketUsed(ticket->id));
    QVERIFY(!m_db->validateFileTicket(hash, "download").has_value());
    // 重复消费失败，非法 id 失败
    QVERIFY(!m_db->markFileTicketUsed(ticket->id));
    QVERIFY(!m_db->markFileTicketUsed(0));
    QVERIFY(!m_db->markFileTicketUsed(-1));

    // ticket_hash UNIQUE：同一摘要不能签发两次
    QVERIFY(!m_db->issueFileTicket(fileId, user->id, "download", hash, 300));
    // 入参兜底
    QVERIFY(!m_db->issueFileTicket(0, user->id, "download", QString(64, 'e'), 300));
    QVERIFY(!m_db->issueFileTicket(fileId, 0, "download", QString(64, 'e'), 300));
    QVERIFY(!m_db->issueFileTicket(fileId, user->id, QString(), QString(64, 'e'), 300));
    QVERIFY(!m_db->issueFileTicket(fileId, user->id, "download", QString(), 300));
    QVERIFY(!m_db->issueFileTicket(fileId, user->id, "download", QString(64, 'e'), 0));
}

void TestDatabaseManager::fileTicketRejectsExpiryAndPrunes()
{
    auto user = m_db->getUserByUsername("testuser");
    QVERIFY(user.has_value());
    const qint64 fileId = makeUpload(*m_db, user->id, "tk02");
    const QString hash = QString(64, 'f');

    // ttlSeconds 必须为正，故用 SQL 把 expires_at 拨到过去来模拟过期
    QVERIFY(m_db->issueFileTicket(fileId, user->id, "download", hash, 300));
    QVERIFY(m_db->validateFileTicket(hash, "download").has_value());

    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare("UPDATE file_tickets SET expires_at = datetime('now', '-1 seconds') "
              "WHERE ticket_hash = ?");
    q.addBindValue(hash);
    QVERIFY(q.exec());

    // 过期即拒绝，且不向调用方区分"过期"与"不存在"（避免被用来探测票据库）
    QVERIFY(!m_db->validateFileTicket(hash, "download").has_value());

    QVERIFY(m_db->pruneExpiredFileTickets() >= 1);
    // 清理后再扫一次应无过期票据（其余票据 TTL 均在未来）
    QCOMPARE(m_db->pruneExpiredFileTickets(), 0);
}

void TestDatabaseManager::fileAccessRequiresMessageReference()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    auto outsider = m_db->getUserByUsername("outsider");
    QVERIFY(user1.has_value() && user2.has_value() && outsider.has_value());

    const qint64 fileId = makeUpload(*m_db, user1->id, "ac01");
    QVERIFY(m_db->markFileReady(fileId));

    // 上传者本人始终可访问（发送失败重试、本人其他设备同步）
    QVERIFY(m_db->canUserAccessFile(fileId, user1->id));
    // 尚未被任何消息引用时，他人不可访问（否则可凭 fileId 枚举下载）
    QVERIFY(!m_db->canUserAccessFile(fileId, user2->id));
    QVERIFY(!m_db->canUserAccessFile(fileId, outsider->id));
    QVERIFY(!m_db->isFileReferencedByMessage(fileId));

    // 文件被消息带进会话后，会话成员即可访问
    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    QVERIFY(convId > 0);
    const qint64 msgId = m_db->sendMessage(convId, user1->id, "manifest-cipher", "e2ee",
                                           "cmid-file-ac01", "deviceA", fileId);
    QVERIFY(msgId > 0);
    QVERIFY(m_db->isFileReferencedByMessage(fileId));
    QVERIFY(m_db->canUserAccessFile(fileId, user2->id));
    QVERIFY(m_db->canUserAccessFile(fileId, user1->id));
    // 非会话成员仍不可访问
    QVERIFY(!m_db->canUserAccessFile(fileId, outsider->id));

    // 消息被软删除后，接收方的访问权随之收回（附件不再属于该会话）
    QVERIFY(m_db->deleteMessage(msgId));
    QVERIFY(!m_db->canUserAccessFile(fileId, user2->id));
    QVERIFY(!m_db->isFileReferencedByMessage(fileId));
    // 上传者本人不受影响
    QVERIFY(m_db->canUserAccessFile(fileId, user1->id));

    QVERIFY(!m_db->canUserAccessFile(0, user1->id));
    QVERIFY(!m_db->canUserAccessFile(fileId, 0));
    QVERIFY(!m_db->canUserAccessFile(-1, -1));
    QVERIFY(!m_db->isFileReferencedByMessage(0));
}

void TestDatabaseManager::messageCarriesFileIdAcrossReadPaths()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value() && user2.has_value());
    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    QVERIFY(convId > 0);

    // 普通消息不传 fileId，读出为 0（客户端据此判定正文不是清单）
    const qint64 textId = m_db->sendMessage(convId, user1->id, "plain text", "e2ee",
                                            "cmid-text-nofile", "deviceA");
    QVERIFY(textId > 0);
    const auto textMsg = m_db->getMessage(textId);
    QVERIFY(textMsg.has_value());
    QCOMPARE(textMsg->fileId, qint64(0));

    const qint64 fileId = makeUpload(*m_db, user1->id, "mf01");
    QVERIFY(m_db->markFileReady(fileId));
    const qint64 msgId = m_db->sendMessage(convId, user1->id, "manifest-cipher", "e2ee",
                                           "cmid-file-mf01", "deviceA", fileId);
    QVERIFY(msgId > 0);

    // 四条读取路径口径一致：任一遗漏都会让离线补收的文件消息退化为文本消息
    const auto direct = m_db->getMessage(msgId);
    QVERIFY(direct.has_value());
    QCOMPARE(direct->fileId, fileId);

    const auto byKey = m_db->getMessageByClientKey(user1->id, "deviceA", "cmid-file-mf01");
    QVERIFY(byKey.has_value());
    QCOMPARE(byKey->id, msgId);
    QCOMPARE(byKey->fileId, fileId);

    bool inHistory = false;
    for (const auto &m : m_db->getMessages(convId)) {
        if (m.id == msgId) {
            inHistory = true;
            QCOMPARE(m.fileId, fileId);
        } else if (m.id == textId) {
            QCOMPARE(m.fileId, qint64(0));
        }
    }
    QVERIFY(inHistory);

    bool inSync = false;
    for (const auto &m : m_db->syncMessages(convId, 0, 500)) {
        if (m.id == msgId) {
            inSync = true;
            QCOMPARE(m.fileId, fileId);
        }
    }
    QVERIFY(inSync);
}

void TestDatabaseManager::deleteFileRecordRefusesReferencedFile()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value() && user2.has_value());

    const qint64 fileId = makeUpload(*m_db, user1->id, "dr01");
    QVERIFY(m_db->markFileReady(fileId));
    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    const qint64 msgId = m_db->sendMessage(convId, user1->id, "manifest", "e2ee",
                                           "cmid-file-dr01", "deviceA", fileId);
    QVERIFY(msgId > 0);

    // 被消息引用的文件不得删除：否则客户端会把清单 JSON 当文本渲染
    QVERIFY(!m_db->deleteFileRecord(fileId));
    QVERIFY(m_db->getFileRecord(fileId).has_value());

    // 未被引用的可删，重复删除幂等（回收任务可无条件调用）
    const qint64 orphan = makeUpload(*m_db, user1->id, "dr02");
    QVERIFY(orphan > 0);
    QVERIFY(m_db->deleteFileRecord(orphan));
    QVERIFY(!m_db->getFileRecord(orphan).has_value());
    QVERIFY(m_db->deleteFileRecord(orphan));

    QVERIFY(!m_db->deleteFileRecord(0));
    QVERIFY(!m_db->deleteFileRecord(-1));
}

void TestDatabaseManager::unreferencedReadyFilesAreReclaimable()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value() && user2.has_value());
    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    QVERIFY(convId > 0);

    // 孤儿：已就绪但从未被任何消息引用（上传完成后发送始终未发生）
    const qint64 orphanId = makeUpload(*m_db, user1->id, "or01");
    QVERIFY(m_db->markFileReady(orphanId));
    // 引用中：消息未删除，任何情况下都不得回收
    const qint64 referencedId = makeUpload(*m_db, user1->id, "or02");
    QVERIFY(m_db->markFileReady(referencedId));
    const qint64 msgId = m_db->sendMessage(convId, user1->id, "manifest-cipher", "e2ee",
                                           "cmid-file-or02", "deviceA", referencedId);
    QVERIFY(msgId > 0);

    // 刚完成的上传不算超期：宽限期从 completed_at 起算。续传跨数天才完成的
    // 大文件若按 created_at 判定，会在完成的瞬间被当成孤儿删掉
    for (const FileRecord &rec : m_db->getUnreferencedReadyFiles(48)) {
        QVERIFY(rec.id != orphanId && rec.id != referencedId);
    }

    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare("UPDATE files SET created_at = datetime('now', '-72 hours'), "
              "completed_at = datetime('now', '-72 hours') WHERE id IN (?, ?)");
    q.addBindValue(orphanId);
    q.addBindValue(referencedId);
    QVERIFY(q.exec());

    bool sawOrphan = false;
    for (const FileRecord &rec : m_db->getUnreferencedReadyFiles(48)) {
        QCOMPARE(rec.status, QString("ready"));
        QVERIFY(!m_db->isFileReferencedByMessage(rec.id));
        // 引用中的文件绝不能入选：删它等于把接收端的附件凭空抽走
        QVERIFY(rec.id != referencedId);
        if (rec.id == orphanId) {
            sawOrphan = true;
        }
    }
    QVERIFY(sawOrphan);
    QCOMPARE(m_db->getUnreferencedReadyFiles(48, 1).size(), qsizetype(1));

    // 被引用的文件不得迁移
    QVERIFY(!m_db->cancelUnreferencedReadyFile(referencedId));
    QCOMPARE(m_db->getFileRecord(referencedId)->status, QString("ready"));

    // 孤儿迁入终态，随后由终态那一轮删盘删行
    QVERIFY(m_db->cancelUnreferencedReadyFile(orphanId));
    QCOMPARE(m_db->getFileRecord(orphanId)->status, QString("cancelled"));
    bool handedOff = false;
    for (const FileRecord &rec : m_db->getTerminalFiles(48)) {
        if (rec.id == orphanId) {
            handedOff = true;
        }
    }
    QVERIFY(handedOff);

    // 重复迁移失败（已不是 ready），非法入参与不存在的行同样失败
    QVERIFY(!m_db->cancelUnreferencedReadyFile(orphanId));
    QVERIFY(!m_db->cancelUnreferencedReadyFile(0));
    QVERIFY(!m_db->cancelUnreferencedReadyFile(-1));
    QVERIFY(!m_db->cancelUnreferencedReadyFile(99999999));

    // 消息被软删除后，其附件转为可回收（引用口径与 isFileReferencedByMessage 一致）
    QVERIFY(m_db->deleteMessage(msgId));
    QVERIFY(!m_db->isFileReferencedByMessage(referencedId));
    bool sawReferenced = false;
    for (const FileRecord &rec : m_db->getUnreferencedReadyFiles(48)) {
        if (rec.id == referencedId) {
            sawReferenced = true;
        }
    }
    QVERIFY(sawReferenced);
    QVERIFY(m_db->cancelUnreferencedReadyFile(referencedId));
    QCOMPARE(m_db->getFileRecord(referencedId)->status, QString("cancelled"));

    QVERIFY(m_db->getUnreferencedReadyFiles(0).isEmpty());
    QVERIFY(m_db->getUnreferencedReadyFiles(-1).isEmpty());
    QVERIFY(m_db->getUnreferencedReadyFiles(48, 0).isEmpty());
}

void TestDatabaseManager::sendMessageGuardsFileReadyStateAtomically()
{
    auto user1 = m_db->getUserByUsername("testuser");
    auto user2 = m_db->getUserByUsername("user2");
    QVERIFY(user1.has_value() && user2.has_value());
    const qint64 convId = m_db->getOrCreatePrivateConversation(user1->id, user2->id);
    QVERIFY(convId > 0);

    // ready 文件可正常携带发送，且不置位
    const qint64 okId = makeUpload(*m_db, user1->id, "sg01");
    QVERIFY(m_db->markFileReady(okId));
    bool notReady = true;
    const qint64 okMsg = m_db->sendMessage(convId, user1->id, "manifest", "e2ee",
                                           "cmid-file-sg01", "deviceA", okId, &notReady);
    QVERIFY(okMsg > 0);
    QVERIFY(!notReady);
    QCOMPARE(m_db->getMessage(okMsg)->fileId, okId);

    // 文件离开 ready 后（此处走维护任务的真实迁移路径），同一调用不得写入
    // 消息，并必须明确置位，否则调用方只能回误导性的 InternalError
    const qint64 goneId = makeUpload(*m_db, user1->id, "sg02");
    QVERIFY(m_db->markFileReady(goneId));
    QVERIFY(m_db->cancelUnreferencedReadyFile(goneId));
    notReady = false;
    QCOMPARE(m_db->sendMessage(convId, user1->id, "manifest", "e2ee",
                               "cmid-file-sg02", "deviceA", goneId, &notReady),
             qint64(-1));
    QVERIFY(notReady);
    // 不得留下半截消息行（否则客户端会拿到一条指向已取消文件的消息）
    QVERIFY(!m_db->getMessageByClientKey(user1->id, "deviceA", "cmid-file-sg02").has_value());
    QVERIFY(!m_db->isFileReferencedByMessage(goneId));

    // 仍在 uploading（未完成）与根本不存在的 fileId 同样被守卫拦下
    const qint64 pendingId = makeUpload(*m_db, user1->id, "sg03");
    notReady = false;
    QCOMPARE(m_db->sendMessage(convId, user1->id, "manifest", "e2ee",
                               "cmid-file-sg03", "deviceA", pendingId, &notReady),
             qint64(-1));
    QVERIFY(notReady);

    notReady = false;
    QCOMPARE(m_db->sendMessage(convId, user1->id, "manifest", "e2ee",
                               "cmid-file-sg04", "deviceA", 99999999, &notReady),
             qint64(-1));
    QVERIFY(notReady);

    // 普通消息（fileId=0）不走守卫形式，行为与既有口径一致
    notReady = true;
    const qint64 plainMsg = m_db->sendMessage(convId, user1->id, "plain", "e2ee",
                                              "cmid-file-sg05", "deviceA", 0, &notReady);
    QVERIFY(plainMsg > 0);
    QVERIFY(!notReady);
    QCOMPARE(m_db->getMessage(plainMsg)->fileId, qint64(0));

    // 未传 fileNotReady 也不得崩溃
    QVERIFY(m_db->sendMessage(convId, user1->id, "plain", "e2ee",
                              "cmid-file-sg06", "deviceA", 99999999) < 0);

    // 收尾：本用例留下的 uploading 行转终态，不影响其他用例的配额计数
    QVERIFY(m_db->markFileCancelled(pendingId));
}

QTEST_MAIN(TestDatabaseManager)
#include "TestDatabaseManager.moc"
