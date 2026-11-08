/*
 * Minimal UI for the NFC gate (plain Android views, no layout file, no extra libraries).
 *
 *  - every phone : "Join gate" (new phone: gets its credentials over NFC from the gate)
 *  - admin phone : "Add new device", "Remove device", "Remove ALL user devices"
 *  - admin setup : enter ID + key from  python3 make_keys.py phone <MASTER_HEX> <ID>
 *
 * Admin actions only queue a command: the command is sent when you tap the phone to the gate.
 */
package com.example.gate

import android.app.Activity
import android.os.Bundle
import android.text.InputType
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {

    private lateinit var status: TextView
    private lateinit var log: TextView
    private lateinit var adminBox: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
        }
        status = TextView(this).apply { textSize = 18f }
        log = TextView(this)
        root.addView(status)

        root.addView(button("Join gate (this phone is new)") {
            GateState.armJoin()
            show("Join mode is on for 2 minutes. Ask an admin to start enrollment, then tap this phone to the gate.")
        })

        // ---- admin controls (shown only on admin phones) ----
        adminBox = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        adminBox.addView(button("Add new device") {
            GateState.queueCommand(GateState.CMD_ENROLL)
            show("Tap THIS phone to the gate now (within 1 minute), then tap the new phone.")
        })
        val removeId = EditText(this).apply {
            hint = "ID of the device to remove"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        adminBox.addView(removeId)
        adminBox.addView(button("Remove device") {
            val id = removeId.text.toString().toIntOrNull()
            if (id == null || id < 256 || id > 0xFFFE) {
                show("Enter a valid user device ID (256 or higher)")
                return@button
            }
            GateState.queueCommand(GateState.CMD_REMOVE, id)
            show("Tap this phone to the gate to remove device $id.")
        })
        adminBox.addView(button("Remove ALL user devices") {
            GateState.queueCommand(GateState.CMD_CLEAR)
            show("Tap this phone to the gate to remove all user devices.")
        })
        root.addView(adminBox)

        // ---- admin credentials (from make_keys.py) ----
        val credId = EditText(this).apply {
            hint = "Admin ID (from make_keys.py)"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        val credKey = EditText(this).apply { hint = "Admin key (32 hex characters)" }
        root.addView(credId)
        root.addView(credKey)
        root.addView(button("Save admin credentials") {
            val id = credId.text.toString().toIntOrNull()
            if (id == null) {
                show("Enter the admin ID")
                return@button
            }
            try {
                GateState.save(this, id, credKey.text.toString().trim())
                credKey.setText("")
                refresh()
                show("Credentials saved.")
            } catch (e: IllegalArgumentException) {
                show(e.message ?: "Invalid input")
            }
        })

        root.addView(log)
        setContentView(ScrollView(this).apply { addView(root) })
        refresh()
    }

    override fun onResume() {
        super.onResume()
        refresh()
        GateState.listener = { message ->
            runOnUiThread {
                show(message)
                refresh()
            }
        }
    }

    override fun onPause() {
        super.onPause()
        GateState.listener = null
    }

    private fun button(label: String, onClick: () -> Unit): Button =
        Button(this).apply {
            text = label
            setOnClickListener { onClick() }
        }

    private fun show(message: String) {
        log.text = message + "\n\n" + log.text
    }

    private fun refresh() {
        val id = GateState.id(this)
        status.text = if (id < 1) {
            "No credentials yet. Use \"Join gate\" (new phone) or enter admin credentials."
        } else {
            "Device ID: $id (" + (if (GateState.isAdmin(this)) "admin" else "user") + ")"
        }
        adminBox.visibility = if (GateState.isAdmin(this)) View.VISIBLE else View.GONE
    }
}
