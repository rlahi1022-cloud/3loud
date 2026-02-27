#ifndef PROTOCOL_H
#define PROTOCOL_H

/* =========================================================
   PacketType
   - 요청과 응답에서 동일한 type 사용
   - 별도의 *_RESP 타입 없음
   ========================================================= */
typedef enum
{

    /* ================= 인증 ================= */
    PKT_AUTH_REGISTER_REQ   = 0x0001,
    PKT_AUTH_VERIFY_REQ     = 0x0002,
    PKT_AUTH_LOGIN_REQ      = 0x0003,
    PKT_AUTH_LOGOUT_REQ     = 0x0004,
    PKT_AUTH_PWCHANGE_REQ   = 0x0005,
    PKT_AUTH_NAMECHANGE_REQ = 0x0006,

    /* ================= 메시지 ================= */
    PKT_MSG_SEND_REQ         = 0x0010,
    PKT_MSG_POLL_REQ         = 0x0011,
    PKT_MSG_LIST_REQ         = 0x0012,
    PKT_MSG_DELETE_REQ       = 0x0013,
    PKT_MSG_READ_REQ         = 0x0014,
    PKT_MSG_SETTING_GET_REQ  = 0x0015,
    PKT_MSG_SETTING_UPDATE_REQ = 0x0016,

    /* ================= 파일 ================= */
    PKT_FILE_UPLOAD_REQ   = 0x0020,
    PKT_FILE_CHUNK        = 0x0021,
    PKT_FILE_DOWNLOAD_REQ = 0x0022,
    PKT_FILE_DELETE_REQ   = 0x0023,
    PKT_FILE_LIST_REQ     = 0x0024,

    /* ================= 설정 ================= */
    PKT_SETTINGS_GET_REQ    = 0x0030,
    PKT_SETTINGS_SET_REQ    = 0x0031,
    PKT_BLACKLIST_REQ       = 0x0032,
    PKT_SETTINGS_VERIFY_REQ = 0x0033,

    /* ================= 관리자 ================= */
    PKT_ADMIN_USER_LIST_REQ    = 0x0040,
    PKT_ADMIN_USER_INFO_REQ    = 0x0041,
    PKT_ADMIN_STATE_CHANGE_REQ = 0x0042,

} PacketType;


/* =========================================================
   ResultValue
   - 응답 JSON의 "code" 필드에 사용
   ========================================================= */
typedef enum
{

    VALUE_SUCCESS = 0,

    /* 공통 오류 */
    VALUE_ERR_UNKNOWN        = -1,
    VALUE_ERR_DB             = -2,
    VALUE_ERR_SESSION        = -3,
    VALUE_ERR_PERMISSION     = -4,
    VALUE_ERR_INVALID_PACKET = -5,

    /* 회원가입 */
    VALUE_ERR_ID_DUPLICATE   = -10,
    VALUE_ERR_ID_RULE        = -11,
    VALUE_ERR_PW_RULE        = -12,
    VALUE_ERR_EMAIL_VERIFY   = -13,

    /* 로그인 */
    VALUE_ERR_LOGIN_ID       = -20,
    VALUE_ERR_LOGIN_PW       = -21,

    /* 닉네임 */
    VALUE_ERR_NAME_DUPLICATE = -30,

    /* 메시지 */
    VALUE_ERR_MSG_NOT_FOUND  = -40,

    /* 파일 */
    VALUE_ERR_FILE_NOT_FOUND      = -50,
    VALUE_ERR_FILE_SIZE_LIMIT     = -51,
    VALUE_ERR_FILE_QUOTA_EXCEEDED = -52,
    VALUE_ERR_FILE_INPROGRESS     = -53,

    /* 관리자 */
    VALUE_ERR_USER_NOT_FOUND  = -60,
    VALUE_ERR_ALREADY_BANNED  = -61,

    /* 블랙리스트 */
    VALUE_ERR_BLACKLIST_NOT_FOUND = -70,

} ResultValue;

#endif