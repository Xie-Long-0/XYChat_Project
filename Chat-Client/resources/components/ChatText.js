.pragma library

// M7a: 群系统消息结构化正文（{"event": "..."}，与服务端约定取值）转可读文本。
// 聊天区气泡与会话列表的本地预览共用这一份映射：两处各写一份时，
// 新增事件类型只会更新其中一处，出现"列表能看懂、气泡看不懂"的口径漂移
function systemMessageText(content) {
    try {
        var obj = JSON.parse(content)
        if (obj && obj.event) {
            switch (obj.event) {
            case "group_created": return "创建了群组"
            case "member_added": return "新成员加入群聊"
            case "member_removed": return "成员被移出群聊"
            case "member_left": return "成员退出了群聊"
            case "owner_transferred": return "群主已转让"
            default: return content
            }
        }
    } catch (e) {
        // 非 JSON 正文直接展示原文
    }
    return content
}
