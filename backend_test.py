#!/usr/bin/env python3
import requests
import json
import uuid
import time
import os
from dotenv import load_dotenv
import sys

# Load environment variables from frontend/.env to get the backend URL
load_dotenv("frontend/.env")

# Get the backend URL from environment variables
BACKEND_URL = os.environ.get("REACT_APP_BACKEND_URL")
if not BACKEND_URL:
    print("Error: REACT_APP_BACKEND_URL not found in frontend/.env")
    sys.exit(1)

# Ensure the URL has the /api prefix
API_URL = f"{BACKEND_URL}/api"

print(f"Testing backend API at: {API_URL}")

# Test results tracking
test_results = {
    "total": 0,
    "passed": 0,
    "failed": 0,
    "tests": []
}

def run_test(test_name, test_func):
    """Run a test and track results"""
    test_results["total"] += 1
    print(f"\n{'='*80}\nRunning test: {test_name}\n{'='*80}")
    
    try:
        result = test_func()
        if result:
            test_results["passed"] += 1
            status = "PASSED"
        else:
            test_results["failed"] += 1
            status = "FAILED"
    except Exception as e:
        test_results["failed"] += 1
        status = f"ERROR: {str(e)}"
    
    test_results["tests"].append({
        "name": test_name,
        "status": status
    })
    
    print(f"Test {test_name}: {status}")
    return result

def test_root_endpoint():
    """Test the root API endpoint"""
    response = requests.get(f"{API_URL}/")
    print(f"Response: {response.status_code} - {response.text}")
    return response.status_code == 200 and "RetroOS Backend API" in response.text

def test_system_info():
    """Test the system info endpoint"""
    response = requests.get(f"{API_URL}/system/info")
    print(f"Response: {response.status_code} - {response.text}")
    data = response.json()
    return (response.status_code == 200 and 
            "os_name" in data and 
            "version" in data and 
            "build" in data)

def test_user_registration():
    """Test user registration"""
    username = f"testuser_{uuid.uuid4().hex[:8]}"
    password = "testpassword123"
    
    response = requests.post(
        f"{API_URL}/register",
        json={"username": username, "password": password}
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    data = response.json()
    if response.status_code != 200 or "user_id" not in data:
        return False
    
    # Store user_id for later tests
    global test_user_id
    test_user_id = data["user_id"]
    global test_username
    test_username = username
    global test_password
    test_password = password
    
    return True

def test_duplicate_registration():
    """Test registering with an existing username"""
    response = requests.post(
        f"{API_URL}/register",
        json={"username": test_username, "password": "anotherpassword"}
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    # Should fail with 400 status code
    return response.status_code == 400 and "already exists" in response.text.lower()

def test_user_login():
    """Test user login with valid credentials"""
    response = requests.post(
        f"{API_URL}/login",
        json={"username": test_username, "password": test_password}
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    data = response.json()
    return (response.status_code == 200 and 
            "user_id" in data and 
            data["user_id"] == test_user_id and
            "desktop_settings" in data)

def test_invalid_login():
    """Test login with invalid credentials"""
    response = requests.post(
        f"{API_URL}/login",
        json={"username": test_username, "password": "wrongpassword"}
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    # Should fail with 401 status code
    return response.status_code == 401 and "invalid credentials" in response.text.lower()

def test_get_user():
    """Test getting user information"""
    response = requests.get(f"{API_URL}/user/{test_user_id}")
    print(f"Response: {response.status_code} - {response.text}")
    
    data = response.json()
    return (response.status_code == 200 and 
            "id" in data and 
            data["id"] == test_user_id and
            "username" in data and
            data["username"] == test_username)

def test_get_nonexistent_user():
    """Test getting a user that doesn't exist"""
    fake_id = str(uuid.uuid4())
    response = requests.get(f"{API_URL}/user/{fake_id}")
    print(f"Response: {response.status_code} - {response.text}")
    
    # Should fail with 404 status code
    return response.status_code == 404 and "not found" in response.text.lower()

def test_update_desktop_settings():
    """Test updating desktop settings"""
    settings = {
        "wallpaper": "blue_mountains.jpg",
        "window_positions": {"calculator": {"x": 100, "y": 200}},
        "taskbar_settings": {"position": "top"}
    }
    
    response = requests.put(
        f"{API_URL}/user/{test_user_id}/desktop-settings",
        json=settings
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    if response.status_code != 200:
        return False
    
    # Verify settings were updated
    response = requests.get(f"{API_URL}/user/{test_user_id}")
    data = response.json()
    
    return (response.status_code == 200 and 
            "desktop_settings" in data and
            data["desktop_settings"]["wallpaper"] == "blue_mountains.jpg" and
            data["desktop_settings"]["taskbar_settings"]["position"] == "top")

def test_default_folders():
    """Test that default folders were created during registration"""
    response = requests.get(f"{API_URL}/files/{test_user_id}")
    print(f"Response: {response.status_code} - {response.text}")
    
    if response.status_code != 200:
        return False
    
    folders = response.json()
    folder_names = [folder["name"] for folder in folders]
    
    # Check if all default folders exist
    required_folders = ["Documents", "Desktop", "Downloads", "Programs"]
    return all(folder in folder_names for folder in required_folders)

def test_create_file():
    """Test creating a new file"""
    file_data = {
        "name": "test_file.txt",
        "type": "file",
        "content": "This is a test file content",
        "parent_id": None,
        "path": "/test_file.txt"
    }
    
    response = requests.post(
        f"{API_URL}/files/{test_user_id}",
        json=file_data
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    data = response.json()
    if response.status_code != 200 or "id" not in data:
        return False
    
    # Store file_id for later tests
    global test_file_id
    test_file_id = data["id"]
    
    return True

def test_create_folder():
    """Test creating a new folder"""
    folder_data = {
        "name": "test_folder",
        "type": "folder",
        "parent_id": None,
        "path": "/test_folder"
    }
    
    response = requests.post(
        f"{API_URL}/files/{test_user_id}",
        json=folder_data
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    data = response.json()
    if response.status_code != 200 or "id" not in data:
        return False
    
    # Store folder_id for later tests
    global test_folder_id
    test_folder_id = data["id"]
    
    return True

def test_create_nested_file():
    """Test creating a file inside a folder"""
    file_data = {
        "name": "nested_file.txt",
        "type": "file",
        "content": "This is a nested file",
        "parent_id": test_folder_id,
        "path": "/test_folder/nested_file.txt"
    }
    
    response = requests.post(
        f"{API_URL}/files/{test_user_id}",
        json=file_data
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    data = response.json()
    if response.status_code != 200 or "id" not in data:
        return False
    
    # Store nested_file_id for later tests
    global test_nested_file_id
    test_nested_file_id = data["id"]
    
    return True

def test_get_file():
    """Test getting a specific file"""
    response = requests.get(f"{API_URL}/files/{test_user_id}/{test_file_id}")
    print(f"Response: {response.status_code} - {response.text}")
    
    data = response.json()
    return (response.status_code == 200 and 
            "id" in data and 
            data["id"] == test_file_id and
            "content" in data and
            "This is a test file content" in data["content"])

def test_update_file():
    """Test updating a file"""
    update_data = {
        "name": "updated_file.txt",
        "content": "This content has been updated"
    }
    
    response = requests.put(
        f"{API_URL}/files/{test_user_id}/{test_file_id}",
        json=update_data
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    if response.status_code != 200:
        return False
    
    # Verify file was updated
    response = requests.get(f"{API_URL}/files/{test_user_id}/{test_file_id}")
    data = response.json()
    
    return (response.status_code == 200 and 
            data["name"] == "updated_file.txt" and
            data["content"] == "This content has been updated")

def test_list_files():
    """Test listing all files for a user"""
    response = requests.get(f"{API_URL}/files/{test_user_id}")
    print(f"Response: {response.status_code} - {response.text}")
    
    if response.status_code != 200:
        return False
    
    files = response.json()
    file_ids = [file["id"] for file in files]
    
    # Check if our created files are in the list
    return test_file_id in file_ids and test_folder_id in file_ids

def test_list_files_by_parent():
    """Test listing files by parent folder"""
    response = requests.get(f"{API_URL}/files/{test_user_id}?parent_id={test_folder_id}")
    print(f"Response: {response.status_code} - {response.text}")
    
    if response.status_code != 200:
        return False
    
    files = response.json()
    file_ids = [file["id"] for file in files]
    
    # Check if our nested file is in the list
    return test_nested_file_id in file_ids

def test_list_files_by_path():
    """Test listing files by path"""
    response = requests.get(f"{API_URL}/files/{test_user_id}?path=/test_folder")
    print(f"Response: {response.status_code} - {response.text}")
    
    if response.status_code != 200:
        return False
    
    files = response.json()
    paths = [file["path"] for file in files]
    
    # Check if our path is in the results
    return any("/test_folder" in path for path in paths)

def test_delete_file():
    """Test deleting a file"""
    response = requests.delete(f"{API_URL}/files/{test_user_id}/{test_file_id}")
    print(f"Response: {response.status_code} - {response.text}")
    
    if response.status_code != 200:
        return False
    
    # Verify file was deleted
    response = requests.get(f"{API_URL}/files/{test_user_id}/{test_file_id}")
    
    # Should return 404 Not Found
    return response.status_code == 404

def test_user_isolation():
    """Test that users can't access each other's files"""
    # Create a second user
    username2 = f"testuser2_{uuid.uuid4().hex[:8]}"
    password2 = "testpassword456"
    
    response = requests.post(
        f"{API_URL}/register",
        json={"username": username2, "password": password2}
    )
    
    if response.status_code != 200:
        print(f"Failed to create second user: {response.text}")
        return False
    
    user2_id = response.json()["user_id"]
    
    # Create a file for the second user
    file_data = {
        "name": "user2_file.txt",
        "type": "file",
        "content": "This belongs to user 2",
        "parent_id": None,
        "path": "/user2_file.txt"
    }
    
    response = requests.post(
        f"{API_URL}/files/{user2_id}",
        json=file_data
    )
    
    if response.status_code != 200:
        print(f"Failed to create file for second user: {response.text}")
        return False
    
    user2_file_id = response.json()["id"]
    
    # Try to access user2's file as user1
    response = requests.get(f"{API_URL}/files/{test_user_id}/{user2_file_id}")
    
    # Should return 404 Not Found
    return response.status_code == 404

def run_all_tests():
    """Run all tests in sequence"""
    # System tests
    run_test("Root Endpoint", test_root_endpoint)
    run_test("System Info", test_system_info)
    
    # User authentication tests
    run_test("User Registration", test_user_registration)
    run_test("Duplicate Registration", test_duplicate_registration)
    run_test("User Login", test_user_login)
    run_test("Invalid Login", test_invalid_login)
    run_test("Get User", test_get_user)
    run_test("Get Nonexistent User", test_get_nonexistent_user)
    run_test("Update Desktop Settings", test_update_desktop_settings)
    
    # File system tests
    run_test("Default Folders", test_default_folders)
    run_test("Create File", test_create_file)
    run_test("Create Folder", test_create_folder)
    run_test("Create Nested File", test_create_nested_file)
    run_test("Get File", test_get_file)
    run_test("Update File", test_update_file)
    run_test("List Files", test_list_files)
    run_test("List Files by Parent", test_list_files_by_parent)
    run_test("List Files by Path", test_list_files_by_path)
    run_test("Delete File", test_delete_file)
    
    # Security tests
    run_test("User Isolation", test_user_isolation)

def print_summary():
    """Print a summary of test results"""
    print(f"\n{'='*80}")
    print(f"TEST SUMMARY")
    print(f"{'='*80}")
    print(f"Total tests: {test_results['total']}")
    print(f"Passed: {test_results['passed']}")
    print(f"Failed: {test_results['failed']}")
    print(f"Success rate: {(test_results['passed'] / test_results['total']) * 100:.2f}%")
    print(f"{'='*80}")
    
    # Print individual test results
    for test in test_results["tests"]:
        status_symbol = "✅" if "PASSED" in test["status"] else "❌"
        print(f"{status_symbol} {test['name']}: {test['status']}")

if __name__ == "__main__":
    run_all_tests()
    print_summary()