from datetime import datetime

from azure.eventhub import EventHubConsumerClient
import json
from cassandra_client import CassandraClient
from azure.iot.hub import IoTHubRegistryManager
import joblib
import pandas as pd

# Load the machine learning model
loaded_model = joblib.load('model/isolation_forest_model.joblib')



IoTHubConnectionString = ""
registry_manager = IoTHubRegistryManager.from_connection_string(IoTHubConnectionString)

# CREDENTIALS INFORMATION

EVENTHUB_COMPATIBLE_ENDPOINT = ""
EVENTHUB_COMPATIBLE_PATH = ""
IOTHUB_SAS_KEY = ""
CONNECTION_STR = ""


db_client = CassandraClient(
        cluster=["localhost"],
        username="admin",
        password="admin",
    )

session = db_client.get_session("iot_data")

cql = """insert into iot_data.air_quality (deviceid, timestamp, ch4, co, co2, dustdensity, humidity, lpg, nh4, smoke, temperature) 
VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)"""

def convert_unix_to_cassandra_timestamp(unix_timestamp):
    # Convert unix timestamp to Cassandra datetime string
    dt_object = datetime.fromtimestamp(unix_timestamp)
    cassandra_timestamp = dt_object.strftime('%Y-%m-%d %H:%M:%S')
    return cassandra_timestamp

def alert_device(deviceID):
    message = '''{
      "command": "toggleLED"
    }'''
    registry_manager.send_c2d_message(deviceID, message, {})


def on_event(partition_context, event):
    print("Telemetry received: ", event.body_as_str())
    print()
    message_str = str(event.body_as_str())
    json_mess = json.loads(message_str.replace("'", "\""))
    predict_input = pd.DataFrame([json_mess]).rename(columns={"dustDensity": "dustdensity"})[['co','dustdensity','lpg','smoke']]
    predictions = loaded_model.predict(predict_input)
    if predictions == -1:
        alert_device(json_mess["deviceid"])
        print("Alert sent to device: " , json_mess["deviceid"])
    print("Inserting data into Cassandra.")
    session.execute(
        cql,
        (json_mess["deviceid"], json_mess["timestamp"]*1000, json_mess["ch4"],
         json_mess["co"], json_mess["co2"], json_mess["dustdensity"], json_mess["humidity"],
         json_mess["lpg"], json_mess["nh4"], json_mess["smoke"], json_mess["temperature"])
    )
    print("Data inserted into Cassandra.")

    partition_context.update_checkpoint()


def on_error(partition_context, error):
    if partition_context:
        print("An exception: {} occurred during receiving from Partition: {}.".format(
            error,
            partition_context.partition_id
        ))
    else:
        print("An exception: {} occurred during the load balance process.".format(error))


def main():

    client = EventHubConsumerClient.from_connection_string(
        conn_str=CONNECTION_STR,
        consumer_group="$default"
    )
    try:
        with client:
            client.receive(on_event=on_event, on_error=on_error)


    except KeyboardInterrupt:
        print("Receiving has stopped.")

if __name__ == '__main__':
    main()