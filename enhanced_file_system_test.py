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

print(f"Testing enhanced file system at: {API_URL}")

# Test results tracking
test_results = {
    "total": 0,
    "passed": 0,
    "failed": 0,
    "tests": []
}

# Global variables to store test data
test_user_id = None
test_username = None
test_password = None
documents_folder_id = None
subfolder_id = None
test_file_id = None
subfolder_file_id = None

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

def test_user_registration():
    """Test user registration and verify default folders are created"""
    username = f"enhanced_test_{uuid.uuid4().hex[:8]}"
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
    
    # Verify default folders were created
    response = requests.get(f"{API_URL}/files/{test_user_id}")
    if response.status_code != 200:
        print("Failed to get files for new user")
        return False
    
    folders = response.json()
    folder_names = [folder["name"] for folder in folders]
    
    # Check if all default folders exist
    required_folders = ["Documents", "Desktop", "Downloads", "Programs", "Recycle Bin"]
    if not all(folder in folder_names for folder in required_folders):
        print(f"Missing some default folders. Found: {folder_names}")
        return False
    
    # Store Documents folder ID for later tests
    for folder in folders:
        if folder["name"] == "Documents":
            global documents_folder_id
            documents_folder_id = folder["id"]
            break
    
    return True

def test_create_file_in_documents():
    """Test creating a text file in Documents folder"""
    file_data = {
        "name": "document1.txt",
        "type": "file",
        "content": "This is a test document in the Documents folder",
        "parent_id": documents_folder_id,
        "path": "/Documents/document1.txt"
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
    
    # Verify file was created with correct properties
    if (data["name"] != "document1.txt" or 
        data["type"] != "file" or 
        data["parent_id"] != documents_folder_id or
        data["path"] != "/Documents/document1.txt" or
        data["file_extension"] != "txt"):
        print(f"File properties don't match expected values: {data}")
        return False
    
    return True

def test_create_subfolder_in_documents():
    """Test creating a subfolder in Documents"""
    folder_data = {
        "name": "SubFolder",
        "type": "folder",
        "parent_id": documents_folder_id,
        "path": "/Documents/SubFolder"
    }
    
    response = requests.post(
        f"{API_URL}/files/{test_user_id}",
        json=folder_data
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    data = response.json()
    if response.status_code != 200 or "id" not in data:
        return False
    
    # Store subfolder_id for later tests
    global subfolder_id
    subfolder_id = data["id"]
    
    # Verify folder was created with correct properties
    if (data["name"] != "SubFolder" or 
        data["type"] != "folder" or 
        data["parent_id"] != documents_folder_id or
        data["path"] != "/Documents/SubFolder"):
        print(f"Folder properties don't match expected values: {data}")
        return False
    
    return True

def test_create_file_in_subfolder():
    """Test creating a file in the subfolder"""
    file_data = {
        "name": "nested_document.txt",
        "type": "file",
        "content": "This is a nested document in the SubFolder",
        "parent_id": subfolder_id,
        "path": "/Documents/SubFolder/nested_document.txt"
    }
    
    response = requests.post(
        f"{API_URL}/files/{test_user_id}",
        json=file_data
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    data = response.json()
    if response.status_code != 200 or "id" not in data:
        return False
    
    # Store subfolder_file_id for later tests
    global subfolder_file_id
    subfolder_file_id = data["id"]
    
    # Verify file was created with correct properties
    if (data["name"] != "nested_document.txt" or 
        data["type"] != "file" or 
        data["parent_id"] != subfolder_id or
        data["path"] != "/Documents/SubFolder/nested_document.txt" or
        data["file_extension"] != "txt"):
        print(f"File properties don't match expected values: {data}")
        return False
    
    return True

def test_navigate_folder_structure_by_parent():
    """Test navigating through the folder structure using parent_id"""
    # First, get root level folders
    response = requests.get(f"{API_URL}/files/{test_user_id}")
    if response.status_code != 200:
        print(f"Failed to get root folders: {response.text}")
        return False
    
    root_files = response.json()
    root_file_ids = [file["id"] for file in root_files]
    
    # Verify Documents folder is in root
    if documents_folder_id not in root_file_ids:
        print("Documents folder not found in root")
        return False
    
    # Navigate to Documents folder
    response = requests.get(f"{API_URL}/files/{test_user_id}?parent_id={documents_folder_id}")
    if response.status_code != 200:
        print(f"Failed to get files in Documents: {response.text}")
        return False
    
    documents_files = response.json()
    documents_file_ids = [file["id"] for file in documents_files]
    
    # Verify our file and subfolder are in Documents
    if test_file_id not in documents_file_ids or subfolder_id not in documents_file_ids:
        print("File or subfolder not found in Documents")
        return False
    
    # Navigate to subfolder
    response = requests.get(f"{API_URL}/files/{test_user_id}?parent_id={subfolder_id}")
    if response.status_code != 200:
        print(f"Failed to get files in subfolder: {response.text}")
        return False
    
    subfolder_files = response.json()
    subfolder_file_ids = [file["id"] for file in subfolder_files]
    
    # Verify our nested file is in subfolder
    if subfolder_file_id not in subfolder_file_ids:
        print("Nested file not found in subfolder")
        return False
    
    return True

def test_navigate_folder_structure_by_path():
    """Test navigating through the folder structure using path"""
    # Get files in Documents folder by path
    response = requests.get(f"{API_URL}/files/{test_user_id}?path=/Documents")
    if response.status_code != 200:
        print(f"Failed to get files in Documents by path: {response.text}")
        return False
    
    documents_files = response.json()
    documents_paths = [file["path"] for file in documents_files]
    
    # Verify our file and subfolder paths are in results
    if "/Documents/document1.txt" not in documents_paths or "/Documents/SubFolder" not in documents_paths:
        print("File or subfolder path not found in Documents")
        return False
    
    # Get files in subfolder by path
    response = requests.get(f"{API_URL}/files/{test_user_id}?path=/Documents/SubFolder")
    if response.status_code != 200:
        print(f"Failed to get files in subfolder by path: {response.text}")
        return False
    
    subfolder_files = response.json()
    subfolder_paths = [file["path"] for file in subfolder_files]
    
    # Verify our nested file path is in results
    if "/Documents/SubFolder/nested_document.txt" not in subfolder_paths:
        print("Nested file path not found in subfolder")
        return False
    
    return True

def test_update_file_content():
    """Test updating file content"""
    update_data = {
        "content": "This content has been updated in the Documents folder"
    }
    
    response = requests.put(
        f"{API_URL}/files/{test_user_id}/{test_file_id}",
        json=update_data
    )
    print(f"Response: {response.status_code} - {response.text}")
    
    if response.status_code != 200:
        return False
    
    # Verify file content was updated
    response = requests.get(f"{API_URL}/files/{test_user_id}/{test_file_id}")
    if response.status_code != 200:
        print(f"Failed to get updated file: {response.text}")
        return False
    
    data = response.json()
    if data["content"] != "This content has been updated in the Documents folder":
        print(f"File content was not updated correctly: {data['content']}")
        return False
    
    return True

def test_file_extension_handling():
    """Test file extension handling for different file types"""
    extensions = {
        "document.txt": "txt",
        "spreadsheet.xlsx": "xlsx",
        "presentation.pptx": "pptx",
        "image.png": "png",
        "code.py": "py",
        "no_extension": ""
    }
    
    for filename, expected_ext in extensions.items():
        file_data = {
            "name": filename,
            "type": "file",
            "content": f"Content for {filename}",
            "parent_id": documents_folder_id,
            "path": f"/Documents/{filename}"
        }
        
        response = requests.post(
            f"{API_URL}/files/{test_user_id}",
            json=file_data
        )
        
        if response.status_code != 200:
            print(f"Failed to create file {filename}: {response.text}")
            return False
        
        data = response.json()
        if data["file_extension"] != expected_ext:
            print(f"Expected extension '{expected_ext}' for {filename}, got '{data['file_extension']}'")
            return False
    
    return True

def run_all_tests():
    """Run all enhanced file system tests in sequence"""
    run_test("User Registration and Default Folders", test_user_registration)
    run_test("Create File in Documents", test_create_file_in_documents)
    run_test("Create Subfolder in Documents", test_create_subfolder_in_documents)
    run_test("Create File in Subfolder", test_create_file_in_subfolder)
    run_test("Navigate Folder Structure by Parent", test_navigate_folder_structure_by_parent)
    run_test("Navigate Folder Structure by Path", test_navigate_folder_structure_by_path)
    run_test("Update File Content", test_update_file_content)
    run_test("File Extension Handling", test_file_extension_handling)

def print_summary():
    """Print a summary of test results"""
    print(f"\n{'='*80}")
    print(f"ENHANCED FILE SYSTEM TEST SUMMARY")
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