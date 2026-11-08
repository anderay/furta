/*
 * Android side of the NFC gate: Host Card Emulation service + shared state.
 * Works together with pn532_gate_auth.ino. Needs minSdk 26 (vibration effect API).
 *
 * Protocol (gate = reader, phone = card)
 *   00 A4 04 00 07 F0010203040506 00   SELECT AID (matched by Android, answered 90 00)
 *   80 10 00 00 08 <challenge[8]>      -> ID[2] CMD[1] ARG[2] MAC[8] 90 00
 *                                         or 6F 01 (this phone wants to join)
 *                                         or 6A 82 (no credentials stored)
 *   80 20 IDhi IDlo 10 <key[16]>       gate gives credentials to a joining phone (90 00)
 *   80 30 <code> 00                    gate reports the result -> shown on the phone
 *   MAC = first 8 bytes of HMAC-SHA256(key, "GATE1" | challenge | ID | CMD | ARG)
 *
 * ---- AndroidManifest.xml ----
 *
 *   <uses-permission android:name="android.permission.NFC" />
 *   <uses-permission android:name="android.permission.VIBRATE" />
 *   <uses-feature android:name="android.hardware.nfc.hce" android:required="true" />
 *
 *   <application ...>
 *       <activity android:name=".MainActivity" android:exported="true">
 *           <intent-filter>
 *               <action android:name="android.intent.action.MAIN" />
 *               <category android:name="android.intent.category.LAUNCHER" />
 *           </intent-filter>
 *       </activity>
 *
 *       <service
 *           android:name=".GateService"
 *           android:exported="true"
 *           android:permission="android.permission.BIND_NFC_SERVICE">
 *           <intent-filter>
 *               <action android:name="android.nfc.cardemulation.action.HOST_APDU_SERVICE" />
 *           </intent-filter>
 *           <meta-data
 *               android:name="android.nfc.cardemulation.host_apdu_service"
 *               android:resource="@xml/apduservice" />
 *       </service>
 *   </application>
 *
 * ---- res/xml/apduservice.xml ----
 *
 *   <host-apdu-service xmlns:android="http://schemas.android.com/apk/res/android"
 *       android:description="@string/gate_service_description"
 *       android:requireDeviceUnlock="false">
 *       <aid-group android:description="@string/gate_aid_description" android:category="other">
 *           <aid-filter android:name="F0010203040506" />
 *       </aid-group>
 *   </host-apdu-service>
 *
 *   (add the two description strings to res/values/strings.xml)
 *
 * requireDeviceUnlock="false": the phone answers with the screen on, even if locked.
 *
 * ---- Credentials ----
 * Admin phones: python3 make_keys.py phone <MASTER_HEX> <ID>, then enter ID and key in the app.
 * User phones: nothing to type. An admin adds them over NFC ("Join gate" on the new phone).
 * The key is kept in the app's private storage. For more protection wrap it with an
 * Android Keystore key (e.g. EncryptedSharedPreferences from androidx.security).
 */
package com.example.gate

import android.content.Context
import android.nfc.cardemulation.HostApduService
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/** State shared by the HCE service and the UI. */
object GateState {
    const val CMD_OPEN = 0
    const val CMD_ENROLL = 1
    const val CMD_REMOVE = 2
    const val CMD_CLEAR = 3

    private const val PREFS = "gate"
    private const val JOIN_WINDOW_MS = 120_000L      // how long "Join gate" stays armed
    private const val COMMAND_WINDOW_MS = 60_000L    // how long an admin command waits for the tap

    private var joinUntil = 0L
    private var pendingCmd = CMD_OPEN
    private var pendingArg = 0
    private var pendingUntil = 0L

    /** Status messages for the UI (set by MainActivity while it is visible). */
    var listener: ((String) -> Unit)? = null

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun id(ctx: Context): Int = prefs(ctx).getInt("id", -1)
    fun keyHex(ctx: Context): String? = prefs(ctx).getString("key", null)
    fun isAdmin(ctx: Context): Boolean = id(ctx) in 1..255

    fun save(ctx: Context, id: Int, keyHex: String) {
        require(id in 1..0xFFFF) { "ID must be 1..65535" }
        require(keyHex.length == 32 && keyHex.all { it in "0123456789abcdefABCDEF" }) {
            "Key must be 32 hex characters"
        }
        prefs(ctx).edit().putInt("id", id).putString("key", keyHex.lowercase()).apply()
    }

    fun armJoin() { joinUntil = SystemClock.elapsedRealtime() + JOIN_WINDOW_MS }
    fun disarmJoin() { joinUntil = 0L }
    fun joinArmed(): Boolean = SystemClock.elapsedRealtime() < joinUntil

    /** Admin: the next tap at the gate carries this command instead of "open". */
    fun queueCommand(cmd: Int, arg: Int = 0) {
        pendingCmd = cmd
        pendingArg = arg
        pendingUntil = SystemClock.elapsedRealtime() + COMMAND_WINDOW_MS
    }

    /** Returns the command for this tap (one-shot) and falls back to "open". */
    fun takeCommand(): Pair<Int, Int> {
        if (SystemClock.elapsedRealtime() < pendingUntil) {
            val result = Pair(pendingCmd, pendingArg)
            pendingCmd = CMD_OPEN
            pendingArg = 0
            pendingUntil = 0L
            return result
        }
        return Pair(CMD_OPEN, 0)
    }

    fun notify(ctx: Context, message: String) {
        vibrate(ctx)
        listener?.invoke(message)
    }

    @Suppress("DEPRECATION")
    private fun vibrate(ctx: Context) {
        val v = ctx.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator ?: return
        if (Build.VERSION.SDK_INT >= 26) {
            v.vibrate(VibrationEffect.createOneShot(150, VibrationEffect.DEFAULT_AMPLITUDE))
        } else {
            v.vibrate(150L)
        }
    }
}

class GateService : HostApduService() {

    companion object {
        private val SW_OK = byteArrayOf(0x90.toByte(), 0x00)
        private val SW_WRONG_LENGTH = byteArrayOf(0x67, 0x00)
        private val SW_CONDITIONS = byteArrayOf(0x69, 0x85.toByte())        // not in join mode
        private val SW_NOT_FOUND = byteArrayOf(0x6A, 0x82.toByte())         // no credentials stored
        private val SW_INS_NOT_SUPPORTED = byteArrayOf(0x6D, 0x00)
        private val SW_JOIN = byteArrayOf(0x6F, 0x01)                       // "I want to join"
        private val LABEL = "GATE1".toByteArray(Charsets.US_ASCII)

        private fun hexToBytes(hex: String): ByteArray =
            ByteArray(hex.length / 2) { hex.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
    }

    override fun processCommandApdu(apdu: ByteArray, extras: Bundle?): ByteArray {
        if (apdu.size < 4) return SW_INS_NOT_SUPPORTED
        val cla = apdu[0].toInt() and 0xFF
        val ins = apdu[1].toInt() and 0xFF
        return when {
            cla == 0x00 && ins == 0xA4 -> SW_OK              // SELECT AID: Android already matched it
            cla == 0x80 && ins == 0x10 -> answerChallenge(apdu)
            cla == 0x80 && ins == 0x20 -> acceptCredentials(apdu)
            cla == 0x80 && ins == 0x30 -> showResult(apdu)
            else -> SW_INS_NOT_SUPPORTED
        }
    }

    override fun onDeactivated(reason: Int) {
        // no state is kept between taps
    }

    private fun answerChallenge(apdu: ByteArray): ByteArray {
        // 80 10 00 00 08 <challenge[8]>
        if (apdu.size < 13 || (apdu[4].toInt() and 0xFF) != 8) return SW_WRONG_LENGTH
        if (GateState.joinArmed()) return SW_JOIN

        val id = GateState.id(this)
        val keyHex = GateState.keyHex(this)
        if (id < 1 || keyHex == null) return SW_NOT_FOUND

        val challenge = apdu.copyOfRange(5, 13)
        val (cmd, arg) = GateState.takeCommand()
        val head = byteArrayOf(
            (id shr 8).toByte(), id.toByte(),
            cmd.toByte(),
            (arg shr 8).toByte(), arg.toByte()
        )
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(hexToBytes(keyHex), "HmacSHA256"))
        val full = mac.doFinal(LABEL + challenge + head)

        return head + full.copyOf(8) + SW_OK                 // ID CMD ARG MAC 90 00
    }

    private fun acceptCredentials(apdu: ByteArray): ByteArray {
        // 80 20 IDhi IDlo 10 <key[16]>
        if (!GateState.joinArmed()) return SW_CONDITIONS
        if (apdu.size < 21 || (apdu[4].toInt() and 0xFF) != 16) return SW_WRONG_LENGTH

        val id = ((apdu[2].toInt() and 0xFF) shl 8) or (apdu[3].toInt() and 0xFF)
        val keyHex = apdu.copyOfRange(5, 21).joinToString("") { "%02x".format(it) }
        GateState.save(this, id, keyHex)
        GateState.disarmJoin()
        GateState.notify(this, "Joined the gate as device $id")
        return SW_OK
    }

    private fun showResult(apdu: ByteArray): ByteArray {
        // 80 30 <code> 00
        val message = when (apdu[2].toInt() and 0xFF) {
            0x01 -> "Gate opened"
            0x02 -> "Enrollment is open for about a minute: now tap the new phone"
            0x03 -> "Device removed"
            0x04 -> "Device not found"
            0x05 -> "All user devices removed"
            0x10 -> "Enrollment is not open: ask an admin"
            0x11 -> "Device list is full"
            0xE0 -> "Not authorized"
            else -> "Gate answered with an unknown code"
        }
        GateState.notify(this, message)
        return SW_OK
    }
}
